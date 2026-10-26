#define LOG_TAG "mpegts"

#include "backends/mp2t/ts_reader.h"

#include <algorithm>

#include "backends/mp2t/elementary_stream.h"
#include "base/log.h"

namespace lychee {

namespace {

// Packets read at a time: 96 KB.
constexpr int64_t kChunkPackets = 512;
// Bytes looked at for the first sync byte.
constexpr int64_t kSyncBytes = 64 * 1024;
constexpr int64_t kWrapTicks = int64_t{1} << 33;
// A larger distance to the next packet is a gap, not a duration.
constexpr int64_t kMaxPacketDuration = 10 * kMicrosPerSecond;
// ADTS frames are at most 8191 bytes; more without one is not ADTS.
constexpr size_t kMaxAdtsBuffer = 64 * 1024;

// Where 2-3 sync bytes |stride| apart start in |data|, or -1.
int64_t FindSync(const uint8_t* data, size_t size, int stride) {
  const size_t prefix = static_cast<size_t>(stride) - kTsPacketSize;
  const size_t count = std::min<size_t>(3, size / static_cast<size_t>(stride));
  if (count < 2) {
    return -1;
  }
  for (size_t i = 0; i < static_cast<size_t>(stride); ++i) {
    bool match = true;
    for (size_t k = 0; k < count && match; ++k) {
      const size_t at = i + prefix + k * static_cast<size_t>(stride);
      match = at < size && data[at] == 0x47;
    }
    if (match) {
      return static_cast<int64_t>(i);
    }
  }
  return -1;
}

void Describe(int stream_type, const TsStreamEntry& entry, StreamInfo* info) {
  info->type = MediaType::kUnknown;
  VideoCodec video = VideoCodec::kUnknown;
  AudioCodec audio = AudioCodec::kUnknown;
  switch (stream_type) {
    case 0x1b:
      video = VideoCodec::kH264;
      break;
    case 0x24:
      video = VideoCodec::kHEVC;
      break;
    case 0x0f:
      audio = AudioCodec::kAAC;  // ADTS
      break;
    case 0x03:
    case 0x04:
      audio = AudioCodec::kMP3;
      break;
    case 0x81:
      audio = AudioCodec::kAC3;
      break;
    case 0x87:
      audio = AudioCodec::kEAC3;
      break;
    case 0x06:
      audio = entry.eac3  ? AudioCodec::kEAC3
              : entry.ac3 ? AudioCodec::kAC3
                          : AudioCodec::kUnknown;
      break;
    default:
      break;
  }
  if (video != VideoCodec::kUnknown) {
    info->type = MediaType::kVideo;
    info->video.codec = video;
    info->video.codec_name = VideoCodecName(video);
  } else if (audio != AudioCodec::kUnknown) {
    info->type = MediaType::kAudio;
    info->audio.codec = audio;
    info->audio.codec_name = AudioCodecName(audio);
  }
  info->language = entry.language;
}

}  // namespace

TsReader::TsReader(DataSource* source) : source_(source), parser_(this) {}
TsReader::~TsReader() = default;

// static
bool TsReader::Detect(const uint8_t* data, size_t size) {
  return FindSync(data, size, kTsPacketSize) == 0 ||
         FindSync(data, size, kTsPacketSize + 4) == 0;
}

// static
int64_t TsReader::Nearest(int64_t ticks, int64_t reference) {
  int64_t delta = (ticks - reference) % kWrapTicks;
  if (delta < 0) {
    delta += kWrapTicks;
  }
  if (delta >= kWrapTicks / 2) {
    delta -= kWrapTicks;
  }
  return reference + delta;
}

void TsReader::SetRange(int64_t begin, int64_t end) {
  begin_ = begin;
  end_ = end;
  buffer_offset_ = read_position_ = begin;
}

bool TsReader::Sync(std::string* error) {
  int64_t want = kSyncBytes;
  if (end_ >= 0) {
    want = std::min(want, end_ - begin_);
  }
  buffer_.resize(static_cast<size_t>(std::max<int64_t>(want, 0)));
  size_t filled = 0;
  while (filled < buffer_.size()) {
    int64_t n = source_->Read(begin_ + static_cast<int64_t>(filled),
                              buffer_.data() + filled, buffer_.size() - filled);
    if (n < 0) {
      *error = source_->error();
      buffer_.clear();
      return false;
    }
    if (n == 0) {
      break;
    }
    filled += static_cast<size_t>(n);
  }
  buffer_.resize(filled);
  for (int stride : {static_cast<int>(kTsPacketSize),
                     static_cast<int>(kTsPacketSize) + 4}) {
    int64_t at = FindSync(buffer_.data(), buffer_.size(), stride);
    if (at >= 0) {
      packet_size_ = stride;
      data_start_ = begin_ + at;
      buffer_.erase(buffer_.begin(), buffer_.begin() + at);
      buffer_offset_ = data_start_;
      read_position_ = begin_ + static_cast<int64_t>(filled);
      if (filled < static_cast<size_t>(want)) {
        eof_ = true;
      }
      return true;
    }
  }
  buffer_.clear();
  *error = "not an MPEG-TS stream (no sync bytes)";
  return false;
}

bool TsReader::ReadMore() {
  if (data_start_ < 0) {
    if (!Sync(&error_)) {
      return false;
    }
  } else {
    int64_t want = packet_size_ * kChunkPackets;
    if (end_ >= 0) {
      want = std::min(want, end_ - read_position_);
    }
    if (want <= 0) {
      eof_ = true;
    } else {
      const size_t old = buffer_.size();
      buffer_.resize(old + static_cast<size_t>(want));
      int64_t n = source_->Read(read_position_, buffer_.data() + old,
                                static_cast<size_t>(want));
      if (n < 0) {
        buffer_.resize(old);
        error_ = source_->error();
        return false;
      }
      buffer_.resize(old + static_cast<size_t>(n));
      read_position_ += n;
      eof_ = n == 0;
    }
  }

  const size_t size = static_cast<size_t>(packet_size_);
  const size_t prefix = size - kTsPacketSize;
  size_t p = 0;
  while (p + size <= buffer_.size()) {
    if (buffer_[p + prefix] != 0x47) {
      // Lost sync: the next place with two sync bytes a packet apart.
      size_t q = p + 1;
      bool found = false;
      for (; q + size + prefix < buffer_.size(); ++q) {
        if (buffer_[q + prefix] == 0x47 && buffer_[q + size + prefix] == 0x47) {
          found = true;
          break;
        }
      }
      if (synced_) {
        LOGW("lost sync at byte %lld", static_cast<long long>(buffer_offset_ + p));
        synced_ = false;
      }
      p = found || !eof_ ? q : buffer_.size();
      if (!found) {
        break;
      }
    }
    synced_ = true;
    parser_.Parse(buffer_.data() + p + prefix,
                  buffer_offset_ + static_cast<int64_t>(p));
    p += size;
  }
  if (eof_) {
    p = buffer_.size();
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<ptrdiff_t>(p));
  buffer_offset_ += static_cast<int64_t>(p);
  if (eof_ && !flushed_) {
    Flush();
  }
  return true;
}

void TsReader::Flush() {
  flushed_ = true;
  parser_.Flush();
  for (StreamState& state : states_) {
    state.adts.clear();
    if (state.pending.valid) {
      state.pending.valid = false;
      if (state.pending.packet.duration <= 0) {
        state.pending.packet.duration = state.last_duration;
      }
      queue_.push_back({std::move(state.pending.packet), state.pending.offset});
    }
  }
}

bool TsReader::Probe(int64_t max_bytes, std::string* error) {
  for (;;) {
    bool done = parser_.has_program() && !streams_.empty();
    for (const TsStream& stream : streams_) {
      done &= stream.configured && stream.first_pts != kNoTimestamp;
    }
    if (done || eof_ || read_position_ - begin_ >= max_bytes) {
      break;
    }
    if (!ReadMore()) {
      *error = error_;
      return false;
    }
  }
  if (!parser_.has_program()) {
    *error = "no MPEG-TS program";
    return false;
  }
  return true;
}

DemuxerReadResult TsReader::Read(EncodedPacket* packet, int64_t* offset) {
  while (queue_.empty()) {
    if (eof_ && flushed_) {
      return DemuxerReadResult::kEndOfStream;
    }
    if (!ReadMore()) {
      return DemuxerReadResult::kError;
    }
  }
  *packet = std::move(queue_.front().packet);
  *offset = queue_.front().offset;
  queue_.pop_front();
  return DemuxerReadResult::kOk;
}

void TsReader::Restart(int64_t offset, int64_t reference) {
  parser_.Reset();
  queue_.clear();
  for (StreamState& state : states_) {
    state.pending.valid = false;
    state.adts.clear();
    state.adts_anchor = kNoTimestamp;
  }
  buffer_.clear();
  if (data_start_ >= 0) {
    offset = std::max(offset, data_start_);
    offset = data_start_ + (offset - data_start_) / packet_size_ * packet_size_;
  } else {
    begin_ = offset;
  }
  buffer_offset_ = read_position_ = offset;
  eof_ = false;
  flushed_ = false;
  synced_ = true;
  reference_ = reference == kNoTimestamp ? kNoTimestamp : MicrosToTicks(reference);
  last_ticks_ = kNoTimestamp;
}

int64_t TsReader::Unwrap(int64_t ticks) {
  const int64_t reference =
      last_ticks_ != kNoTimestamp ? last_ticks_ : reference_;
  last_ticks_ =
      reference == kNoTimestamp ? ticks : Nearest(ticks, reference);
  return last_ticks_;
}

void TsReader::OnProgram(const std::vector<TsStreamEntry>& entries) {
  std::map<int, int> by_pid;
  std::vector<bool> taken(streams_.size(), false);
  std::vector<const TsStreamEntry*> unmatched;
  for (const TsStreamEntry& entry : entries) {
    StreamInfo info;
    Describe(entry.stream_type, entry, &info);
    if (info.type == MediaType::kUnknown) {
      continue;
    }
    auto same = stream_by_pid_.find(entry.pid);
    if (same != stream_by_pid_.end() &&
        streams_[same->second].info.type == info.type) {
      by_pid[entry.pid] = same->second;
      taken[same->second] = true;
    } else {
      unmatched.push_back(&entry);
    }
  }
  for (const TsStreamEntry* entry : unmatched) {
    StreamInfo info;
    Describe(entry->stream_type, *entry, &info);
    int index = -1;
    for (size_t i = 0; i < streams_.size(); ++i) {
      if (!taken[i] && streams_[i].info.type == info.type) {
        index = static_cast<int>(i);
        break;
      }
    }
    if (index < 0) {
      index = static_cast<int>(streams_.size());
      streams_.emplace_back();
      states_.emplace_back();
      taken.push_back(false);
      info.index = index;
      streams_[index].info = info;
    } else {
      LOGI("stream %d: PID %d -> %d", index, streams_[index].pid, entry->pid);
      StreamInfo& existing = streams_[index].info;
      existing.video.codec = info.video.codec;
      existing.video.codec_name = info.video.codec_name;
      existing.audio.codec = info.audio.codec;
      existing.audio.codec_name = info.audio.codec_name;
    }
    taken[index] = true;
    streams_[index].pid = entry->pid;
    by_pid[entry->pid] = index;
  }
  stream_by_pid_ = std::move(by_pid);
}

void TsReader::OnPes(const PesPacket& pes) {
  auto it = stream_by_pid_.find(pes.pid);
  if (it == stream_by_pid_.end()) {
    return;
  }
  const int index = it->second;
  TsStream& stream = streams_[index];
  const int64_t pts =
      pes.pts >= 0 ? TicksToMicros(Unwrap(pes.pts)) : kNoTimestamp;
  const int64_t dts =
      pes.dts >= 0 ? TicksToMicros(Unwrap(pes.dts)) : pts;

  if (stream.info.type == MediaType::kAudio &&
      stream.info.audio.codec == AudioCodec::kAAC) {
    SplitAdts(index, pes, pts);
    return;
  }
  EncodedPacket packet;
  packet.stream_index = index;
  packet.pts = pts;
  packet.dts = dts;
  packet.keyframe = true;
  if (stream.info.type == MediaType::kVideo) {
    AccessUnitInfo au;
    const bool hevc = stream.info.video.codec == VideoCodec::kHEVC;
    ParseAccessUnit(pes.data, pes.size, hevc, &au);
    if (au.has_sps && !stream.configured) {
      VideoConfig& video = stream.info.video;
      video.width = au.width;
      video.height = au.height;
      video.sar_num = au.sar_num;
      video.sar_den = au.sar_den;
      video.color = au.color;
      stream.configured = true;
    }
    // Some encoders mark recovery points (open GOPs, intra refresh)
    // only with the random access indicator and fresh parameter sets.
    packet.keyframe = au.keyframe || (pes.random_access && au.has_sps);
  } else {
    stream.configured = true;
  }
  if (pts == kNoTimestamp) {
    return;  // Cannot be scheduled.
  }
  packet.buffer = PacketBuffer::Copy(pes.data, pes.size);
  Emit(index, std::move(packet), pes.offset);
}

void TsReader::SplitAdts(int index, const PesPacket& pes, int64_t pts) {
  TsStream& stream = streams_[index];
  StreamState& state = states_[index];
  const size_t boundary = state.adts.size();
  if (boundary == 0) {
    state.adts_offset = pes.offset;
  }
  state.adts.insert(state.adts.end(), pes.data, pes.data + pes.size);
  std::vector<uint8_t>& data = state.adts;
  bool anchored = false;
  size_t position = 0;
  while (position + 7 <= data.size()) {
    AdtsHeader header;
    if (!ParseAdtsHeader(&data[position], data.size() - position, &header)) {
      ++position;
      continue;
    }
    if (position + header.frame_length > data.size()) {
      break;
    }
    // The PES timestamp belongs to the first frame that starts in it.
    if (!anchored && position >= boundary && pts != kNoTimestamp) {
      state.adts_anchor = pts;
      state.adts_frames = 0;
      anchored = true;
    }
    if (state.adts_anchor != kNoTimestamp) {
      if (!stream.configured) {
        stream.info.audio.sample_rate = header.sample_rate;
        stream.info.audio.channels = header.channels;
        stream.configured = true;
      }
      // adts_frames counts samples since the anchor.
      const int64_t rate = header.sample_rate;
      const int64_t begin =
          state.adts_anchor + state.adts_frames * kMicrosPerSecond / rate;
      state.adts_frames += header.samples;
      const int64_t end =
          state.adts_anchor + state.adts_frames * kMicrosPerSecond / rate;
      EncodedPacket packet;
      packet.stream_index = index;
      packet.pts = packet.dts = begin;
      packet.duration = end - begin;
      packet.keyframe = true;
      packet.buffer = PacketBuffer::Copy(&data[position], header.frame_length);
      Emit(index, std::move(packet),
           position < boundary ? state.adts_offset : pes.offset);
    }
    position += header.frame_length;
  }
  data.erase(data.begin(), data.begin() + static_cast<ptrdiff_t>(position));
  if (data.size() > kMaxAdtsBuffer) {
    data.clear();
  }
  state.adts_offset = pes.offset;
}

void TsReader::Emit(int index, EncodedPacket packet, int64_t offset) {
  TsStream& stream = streams_[index];
  if (stream.first_pts == kNoTimestamp || packet.pts < stream.first_pts) {
    stream.first_pts = packet.pts;
  }
  StreamState& state = states_[index];
  if (state.pending.valid) {
    EncodedPacket& previous = state.pending.packet;
    const int64_t distance = packet.dts - previous.dts;
    if (distance > 0 && distance < kMaxPacketDuration) {
      state.last_duration = distance;
    }
    if (previous.duration <= 0) {
      previous.duration = distance > 0 && distance < kMaxPacketDuration
                              ? distance
                              : state.last_duration;
    }
    state.pending.valid = false;
    queue_.push_back({std::move(previous), state.pending.offset});
  }
  if (packet.duration > 0) {
    queue_.push_back({std::move(packet), offset});
    return;
  }
  state.pending.packet = std::move(packet);
  state.pending.offset = offset;
  state.pending.valid = true;
}

}  // namespace lychee
