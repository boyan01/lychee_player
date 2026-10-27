#define LOG_TAG "mpegts"

#include "backends/mp2t/ts_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/log.h"

namespace lychee {

namespace {

// How much of the input the open may read to find every stream's
// configuration (FFmpeg reads up to 5 MB).
constexpr int64_t kProbeBytes = 4 << 20;
// Bytes looked at for one timestamp while bisecting, and the range at
// which bisection stops and reading takes over.
constexpr int64_t kPtsWindow = 256 << 10;
constexpr int64_t kBisectStop = 512 << 10;
// Tail looked at for the duration, growing up to the maximum.
constexpr int64_t kTailBytes = 1 << 20;
constexpr int64_t kMaxTailBytes = 16 << 20;

}  // namespace

TsDemuxer::TsDemuxer() = default;
TsDemuxer::~TsDemuxer() = default;

bool TsDemuxer::Open(const std::string& url, std::string* error) {
  std::unique_ptr<DataSource> source = OpenDataSource(url, error);
  if (!source) {
    return false;
  }
  return Open(std::move(source), error);
}

bool TsDemuxer::Open(std::unique_ptr<DataSource> source, std::string* error) {
  {
    std::lock_guard<std::mutex> lock(source_mutex_);
    source_ = std::move(source);
    if (aborted_) {
      source_->Interrupt();
    }
  }
  reader_ = std::make_unique<TsReader>(source_.get());
  reader_->SetRange(0, -1);
  if (!reader_->Probe(kProbeBytes, error)) {
    if (aborted_) {
      *error = "aborted";
    }
    return false;
  }
  media_info_ = MediaInfo();
  media_info_.format_name = "mpegts";
  int64_t start = kNoTimestamp;
  for (const TsStream& stream : reader_->streams()) {
    media_info_.streams.push_back(stream.info);
    if (stream.first_pts != kNoTimestamp &&
        (start == kNoTimestamp || stream.first_pts < start)) {
      start = stream.first_pts;
    }
    const int index = stream.info.index;
    if (stream.info.type == MediaType::kVideo &&
        media_info_.default_video_stream < 0) {
      media_info_.default_video_stream = index;
    }
    if (stream.info.type == MediaType::kAudio &&
        media_info_.default_audio_stream < 0) {
      media_info_.default_audio_stream = index;
    }
  }
  if (media_info_.streams.empty()) {
    *error = "no H.264, HEVC or audio stream in the MPEG-TS program";
    return false;
  }
  media_info_.start_time = start == kNoTimestamp ? 0 : start;
  media_info_.seekable = source_->seekable() && source_->size() > 0;
  if (media_info_.seekable) {
    EstimateDuration();
  }
  enabled_.assign(media_info_.streams.size(), true);
  need_keyframe_.assign(media_info_.streams.size(), true);
  return true;
}

void TsDemuxer::EstimateDuration() {
  const int64_t size = source_->size();
  for (int64_t tail = kTailBytes; tail <= kMaxTailBytes; tail *= 4) {
    const int64_t begin = std::max(reader_->data_start(), size - tail);
    TsReader reader(source_.get());
    reader.SetRange(begin, size);
    int64_t end = kNoTimestamp;
    EncodedPacket packet;
    int64_t offset;
    DemuxerReadResult result;
    while ((result = reader.Read(&packet, &offset)) == DemuxerReadResult::kOk) {
      const int64_t packet_end =
          packet.pts + std::max<int64_t>(packet.duration, 0);
      if (end == kNoTimestamp || packet_end > end) {
        end = packet_end;
      }
    }
    if (end != kNoTimestamp) {
      // Timestamps in the tail may have wrapped past 2^33 since the start.
      const int64_t wrap = TsReader::TicksToMicros(int64_t{1} << 33);
      const int64_t duration =
          ((end - media_info_.start_time) % wrap + wrap) % wrap;
      if (duration > 0) {
        media_info_.duration = duration;
      }
      return;
    }
    // A tail without sync bytes (damage) is no reason to stop looking;
    // a failed read is.
    if (begin == reader_->data_start() ||
        (result == DemuxerReadResult::kError && Failed())) {
      return;
    }
  }
}

void TsDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  std::fill(enabled_.begin(), enabled_.end(), false);
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(enabled_.size())) {
      enabled_[index] = true;
    }
  }
}

bool TsDemuxer::Failed() {
  return aborted_ || interrupted_ || source_->interrupted();
}

DemuxerReadResult TsDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_ || interrupted_) {
      return DemuxerReadResult::kAborted;
    }
    int64_t offset = 0;
    DemuxerReadResult result = reader_->Read(packet, &offset);
    if (result == DemuxerReadResult::kError) {
      if (Failed()) {
        return DemuxerReadResult::kAborted;
      }
      error_ = reader_->error();
      LOGE("read failed: %s", error_.c_str());
      return result;
    }
    if (result != DemuxerReadResult::kOk) {
      return result;
    }
    const int index = packet->stream_index;
    if (index < 0 || index >= static_cast<int>(enabled_.size()) ||
        !enabled_[index]) {
      continue;
    }
    const MediaType type = media_info_.streams[index].type;
    if (type == MediaType::kVideo) {
      if (offset < video_from_offset_ ||
          (need_keyframe_[index] && !packet->keyframe)) {
        continue;
      }
      need_keyframe_[index] = false;
    } else if (audio_from_ != kNoTimestamp &&
               packet->pts + packet->duration <= audio_from_) {
      continue;
    }
    return DemuxerReadResult::kOk;
  }
}

bool TsDemuxer::PtsAt(int64_t offset,
                      int pid,
                      int64_t near,
                      int64_t* pts,
                      bool* read_error) {
  *read_error = false;
  const int64_t size = reader_->packet_size();
  const int64_t start = reader_->data_start();
  offset = start + std::max<int64_t>(offset - start, 0) / size * size;
  // Bisections for a seek's successive back-offs visit the same offsets.
  auto cached = pts_cache_.find(offset);
  if (cached != pts_cache_.end()) {
    if (cached->second < 0) {
      return false;
    }
    *pts = TsReader::TicksToMicros(
        TsReader::Nearest(cached->second, TsReader::MicrosToTicks(near)));
    return true;
  }
  std::vector<uint8_t> window(static_cast<size_t>(kPtsWindow / size * size));
  size_t filled = 0;
  while (filled < window.size()) {
    int64_t n = source_->Read(offset + static_cast<int64_t>(filled),
                              window.data() + filled, window.size() - filled);
    if (n < 0) {
      *read_error = true;
      return false;
    }
    if (n == 0) {
      break;
    }
    filled += static_cast<size_t>(n);
  }
  const size_t prefix = static_cast<size_t>(size) - kTsPacketSize;
  for (size_t p = 0; p + static_cast<size_t>(size) <= filled;) {
    const uint8_t* packet = window.data() + p + prefix;
    if (packet[0] != 0x47) {
      ++p;  // Out of sync: look for the next sync byte.
      continue;
    }
    p += static_cast<size_t>(size);
    const bool unit_start = packet[1] & 0x40;
    const int packet_pid = ((packet[1] & 0x1f) << 8) | packet[2];
    const int adaptation = (packet[3] >> 4) & 3;
    if (!unit_start || packet_pid != pid || !(adaptation & 1)) {
      continue;
    }
    size_t at = 4;
    if (adaptation & 2) {
      at += 1 + packet[4];
    }
    // PES start code, stream id, length, flags, header length, PTS.
    if (at + 14 > kTsPacketSize || packet[at] != 0 || packet[at + 1] != 0 ||
        packet[at + 2] != 1 || !(packet[at + 7] & 0x80)) {
      continue;
    }
    const uint8_t* field = packet + at + 9;
    const int64_t raw = (static_cast<int64_t>((field[0] >> 1) & 7) << 30) |
                        (static_cast<int64_t>(field[1]) << 22) |
                        (static_cast<int64_t>(field[2] >> 1) << 15) |
                        (static_cast<int64_t>(field[3]) << 7) | (field[4] >> 1);
    pts_cache_[offset] = raw;
    *pts = TsReader::TicksToMicros(
        TsReader::Nearest(raw, TsReader::MicrosToTicks(near)));
    return true;
  }
  pts_cache_[offset] = -1;
  return false;
}

int64_t TsDemuxer::Bisect(int pid, int64_t time) {
  const int64_t size = reader_->packet_size();
  int64_t low = reader_->data_start();
  int64_t high = source_->size();
  while (high - low > kBisectStop) {
    int64_t middle = low + (high - low) / 2;
    middle = low + (middle - low) / size * size;
    int64_t pts;
    bool read_error;
    if (!PtsAt(middle, pid, time, &pts, &read_error)) {
      if (read_error) {
        return -1;
      }
      high = middle;
    } else if (pts <= time) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return low;
}

bool TsDemuxer::Seek(int64_t time) {
  interrupted_ = false;
  if (!aborted_) {
    source_->ClearInterrupt();
  }
  if (!media_info_.seekable) {
    return false;
  }
  // Video lands on the keyframe at or before |time|; audio on the frame
  // that plays at that keyframe's time.
  int reference = -1;
  for (const StreamInfo& stream : media_info_.streams) {
    if (enabled_[stream.index] && stream.type == MediaType::kVideo) {
      reference = stream.index;
      break;
    }
  }
  for (const StreamInfo& stream : media_info_.streams) {
    if (reference < 0 && enabled_[stream.index]) {
      reference = stream.index;
    }
  }
  if (reference < 0) {
    reference = 0;
  }
  const bool video = media_info_.streams[reference].type == MediaType::kVideo;
  const int pid = reader_->streams()[reference].pid;
  const int64_t data_start = reader_->data_start();
  pts_cache_.clear();

  int64_t found_offset = -1;
  int64_t found_pts = kNoTimestamp;
  // Audio frames read before the reference's stop: (pts, PES offset).
  std::vector<std::pair<int64_t, int64_t>> audio;
  // Back off further each time the window held no keyframe. Each window
  // reads only up to where the previous one began (plus the audio around
  // a keyframe found there): long GOPs made rereading the whole range a
  // seek's main cost.
  int64_t scanned_from = -1;
  for (int64_t backoff = kMicrosPerSecond;; backoff *= 2) {
    const bool from_start = backoff > 64 * kMicrosPerSecond;
    int64_t start = from_start ? data_start : Bisect(pid, time - backoff);
    if (start < 0) {
      if (!Failed()) {
        error_ = source_->error();
        LOGE("seek failed: %s", error_.c_str());
      }
      return false;
    }
    if (scanned_from >= 0 && start >= scanned_from) {
      if (scanned_from == data_start) {
        break;
      }
      continue;  // Bisection landed in the window already read.
    }
    reader_->Restart(start, time);
    audio.clear();
    EncodedPacket packet;
    int64_t offset;
    for (;;) {
      DemuxerReadResult result = reader_->Read(&packet, &offset);
      if (result == DemuxerReadResult::kError) {
        if (!Failed()) {
          error_ = reader_->error();
          LOGE("seek failed: %s", error_.c_str());
        }
        return false;
      }
      if (result != DemuxerReadResult::kOk) {
        break;
      }
      const int index = packet.stream_index;
      if (index == reference) {
        if (video ? packet.dts > time : packet.pts > time) {
          break;
        }
        if (scanned_from >= 0 && offset >= scanned_from &&
            (found_offset < 0 || packet.pts > found_pts + kMicrosPerSecond)) {
          break;
        }
        if ((!video || packet.keyframe) && packet.pts <= time) {
          found_offset = offset;
          found_pts = packet.pts;
          // Older audio cannot be the frame at a later keyframe.
          std::erase_if(audio, [&](const auto& entry) {
            return entry.first < found_pts - kMicrosPerSecond;
          });
        }
      } else if (index < static_cast<int>(enabled_.size()) && enabled_[index] &&
                 media_info_.streams[index].type == MediaType::kAudio) {
        audio.emplace_back(packet.pts, offset);
      }
    }
    if (found_offset >= 0 || start == data_start) {
      break;
    }
    scanned_from = start;
  }

  std::fill(need_keyframe_.begin(), need_keyframe_.end(), true);
  if (found_offset < 0) {
    // Before the first keyframe: play from the start.
    reader_->Restart(data_start, media_info_.start_time);
    video_from_offset_ = -1;
    audio_from_ = kNoTimestamp;
    return true;
  }
  int64_t start = found_offset;
  int64_t audio_pts = kNoTimestamp;
  for (const auto& [pts, offset] : audio) {
    if (pts <= found_pts && (audio_pts == kNoTimestamp || pts >= audio_pts)) {
      audio_pts = pts;
      start = std::min(found_offset, offset);
    }
  }
  reader_->Restart(start, found_pts);
  video_from_offset_ = video ? found_offset : -1;
  audio_from_ = found_pts;
  return true;
}

void TsDemuxer::Interrupt() {
  interrupted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

void TsDemuxer::Abort() {
  aborted_ = true;
  std::lock_guard<std::mutex> lock(source_mutex_);
  if (source_) {
    source_->Interrupt();
  }
}

}  // namespace lychee
