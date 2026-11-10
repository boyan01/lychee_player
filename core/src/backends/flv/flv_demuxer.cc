#include "backends/flv/flv_demuxer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <utility>

#include "backends/mp2t/elementary_stream.h"
#include "base/log.h"
#include "media/annexb.h"

namespace lychee {
namespace {

constexpr uint32_t FourCC(const char (&s)[5]) {
  return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 |
         uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}

// The largest tag either mode takes (IndexedDemuxer's packet limit).
constexpr uint32_t kMaxTagSize = 16 * 1024 * 1024;
// onMetaData larger than this is skipped unread.
constexpr uint32_t kMaxMetadataSize = 1 << 20;
// Bytes of a payload Inspect() looks at.
constexpr size_t kPayloadHead = 9;
// The open of a stream waits for the configuration of a stream its
// header announces until this much media past the first frame is in...
constexpr int64_t kConfigWait = 2000000;
// ...or this many frames.
constexpr size_t kMaxOpenFrames = 4096;
// What the source is read in.
constexpr size_t kReadSize = 65536;
// Unparsed bytes before this much is dropped from the front of the
// buffer.
constexpr size_t kCompactAt = 1 << 20;
constexpr int64_t kDefaultVideoStep = 33333;

int32_t SI24(const uint8_t* p) {
  int32_t v = (p[0] << 16) | (p[1] << 8) | p[2];
  if (v & 0x800000) v -= 0x1000000;
  return v;
}

int64_t TagTime(const uint8_t* header) {
  // UI24 milliseconds, then their upper 8 bits.
  return (int64_t((header[4] << 16) | (header[5] << 8) | header[6]) |
          int64_t(header[7]) << 24) *
         1000;
}

const char* CodecName(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return "h264";
    case VideoCodec::kHEVC:
      return "hevc";
    default:
      return "av1";
  }
}

// AMF0 (Adobe's Action Message Format 0), as onMetaData carries it.
class Amf {
 public:
  Amf(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool U8(uint8_t* out) {
    if (at_ >= size_) return false;
    *out = data_[at_++];
    return true;
  }
  bool Skip(size_t n) {
    if (size_ - at_ < n) return false;
    at_ += n;
    return true;
  }
  bool Double(double* out) {
    if (size_ - at_ < 8) return false;
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
      bits = bits << 8 | data_[at_ + i];
    memcpy(out, &bits, sizeof(*out));
    at_ += 8;
    return true;
  }
  // A UTF-8 string without its type marker: a 16-bit length, the bytes.
  bool String(std::string* out) {
    if (size_ - at_ < 2) return false;
    const size_t n = (size_t{data_[at_]} << 8) | data_[at_ + 1];
    at_ += 2;
    if (size_ - at_ < n) return false;
    out->assign(reinterpret_cast<const char*>(data_ + at_), n);
    at_ += n;
    return true;
  }
  // Skips one value, marker included; false if malformed or nested deeper
  // than |depth|.
  bool SkipValue(int depth) {
    uint8_t marker;
    if (depth <= 0 || !U8(&marker)) return false;
    std::string ignored;
    switch (marker) {
      case 0:  // number
        return Skip(8);
      case 1:  // boolean
        return Skip(1);
      case 2:  // string
        return String(&ignored);
      case 3:  // object
        return SkipProperties(depth);
      case 5:  // null
      case 6:  // undefined
        return true;
      case 7:  // reference
        return Skip(2);
      case 8:  // ECMA array: a count, then properties as an object's
        return Skip(4) && SkipProperties(depth);
      case 10: {  // strict array
        if (size_ - at_ < 4) return false;
        uint32_t count = 0;
        for (int i = 0; i < 4; ++i)
          count = count << 8 | data_[at_++];
        for (uint32_t i = 0; i < count; ++i)
          if (!SkipValue(depth - 1)) return false;
        return true;
      }
      case 11:  // date: a double, then a time zone
        return Skip(10);
      case 12: {  // long string
        if (size_ - at_ < 4) return false;
        uint32_t n = 0;
        for (int i = 0; i < 4; ++i)
          n = n << 8 | data_[at_++];
        return Skip(n);
      }
      default:
        return false;
    }
  }
  // Name and value pairs up to the empty name and the end marker (9).
  bool SkipProperties(int depth) {
    std::string name;
    while (String(&name)) {
      if (name.empty()) return Skip(1);
      if (!SkipValue(depth - 1)) return false;
    }
    return false;
  }
  bool Peek(uint8_t* out) const {
    if (at_ >= size_) return false;
    *out = data_[at_];
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;
};

}  // namespace

bool FlvDemuxer::Parse() {
  uint8_t header[9];
  if (!Exact(0, header, 9) || !Detect(header, 9)) return false;
  // Known once a read returned.
  if (source_->size() < 0) {
    cache_.clear();
    streaming_ = true;
    return OpenStream();
  }
  const int64_t offset = BE32(header + 5);
  if (offset < 9 || offset > INT32_MAX) return false;
  if (!Exact(offset, header, 4) || BE32(header) != 0) return false;
  if (!ParseIndexed(offset + 4)) return false;
  FinishStreams();
  return true;
}

bool FlvDemuxer::Inspect(uint8_t type,
                         const uint8_t* head,
                         uint32_t size,
                         Payload* out) {
  *out = {};
  if (type == 18) {
    out->kind = Payload::kMetadata;
    return true;
  }
  if (type == 8) {
    if (size < 2) {
      error_ = "flv: truncated audio tag";
      return false;
    }
    if ((head[0] >> 4) != 10) {
      error_ = "flv: only AAC audio is supported";
      return false;
    }
    out->type = MediaType::kAudio;
    out->skip = 2;
    if (head[1] > 1) {
      error_ = "flv: unknown AAC packet type";
      return false;
    }
    out->kind = head[1] == 0 ? Payload::kConfig : Payload::kFrame;
    return true;
  }
  if (type != 9) return true;
  if (size < 1) {
    error_ = "flv: truncated video tag";
    return false;
  }
  out->type = MediaType::kVideo;
  int frame_type;
  if (head[0] & 0x80) {
    // Enhanced RTMP: a frame type of three bits, a packet type, a FourCC.
    frame_type = (head[0] >> 4) & 7;
    const int packet_type = head[0] & 15;
    if (size < 5) {
      error_ = "flv: truncated video tag";
      return false;
    }
    // A command frame (seek start or end) carries no picture.
    if (frame_type == 5) return true;
    switch (BE32(head + 1)) {
      case FourCC("avc1"):
        out->codec = VideoCodec::kH264;
        break;
      case FourCC("hvc1"):
        out->codec = VideoCodec::kHEVC;
        break;
      case FourCC("av01"):
        out->codec = VideoCodec::kAV1;
        break;
      default:
        error_ = "flv: unsupported Enhanced RTMP video codec " +
                 std::string(reinterpret_cast<const char*>(head + 1), 4);
        return false;
    }
    out->skip = 5;
    switch (packet_type) {
      case 0:  // SequenceStart
        out->kind = Payload::kConfig;
        break;
      case 1:  // CodedFrames: AVC and HEVC with a composition time
        out->kind = Payload::kFrame;
        if (out->codec != VideoCodec::kAV1) {
          if (size < 8) {
            error_ = "flv: truncated video tag";
            return false;
          }
          out->cts = int64_t(SI24(head + 5)) * 1000;
          out->skip = 8;
        }
        break;
      case 3:  // CodedFramesX: no composition time
        out->kind = Payload::kFrame;
        break;
      case 2:  // SequenceEnd
      case 4:  // Metadata (HDR colour volume)
        out->kind = Payload::kSkip;
        return true;
      default:
        error_ = "flv: Enhanced RTMP video packet type " +
                 std::to_string(packet_type) + " is not supported";
        return false;
    }
  } else {
    frame_type = head[0] >> 4;
    // Video info / command frames.
    if (frame_type == 5) return true;
    const int codec_id = head[0] & 15;
    if (codec_id == 7) {
      out->codec = VideoCodec::kH264;
    } else if (codec_id == 12) {
      out->codec = VideoCodec::kHEVC;
    } else {
      error_ = "flv: only AVC and HEVC video are supported";
      return false;
    }
    if (size < 5) {
      error_ = "flv: truncated video tag";
      return false;
    }
    out->skip = 5;
    switch (head[1]) {
      case 0:
        out->kind = Payload::kConfig;
        break;
      case 1:
        out->kind = Payload::kFrame;
        out->cts = int64_t(SI24(head + 2)) * 1000;
        break;
      case 2:  // end of sequence
        return true;
      default:
        error_ = "flv: unknown AVC packet type";
        return false;
    }
  }
  out->key = frame_type == 1;
  return true;
}

bool FlvDemuxer::AddConfig(const Payload& payload,
                           std::vector<uint8_t> config) {
  const bool video = payload.type == MediaType::kVideo;
  int& index = video ? video_ : audio_;
  if (index >= 0) {
    // Sent again (a live server's new connection, a repeating encoder):
    // only a different one is a change no decoder here takes mid-stream.
    const StreamInfo& old = info_.streams[static_cast<size_t>(index)];
    if (video
            ? old.video.codec == payload.codec && old.video.extradata == config
            : old.audio.extradata == config)
      return true;
    error_ = video ? "flv: changing video configuration"
                   : "flv: changing AAC configuration";
    return false;
  }
  StreamInfo s;
  int64_t step = kDefaultVideoStep;
  if (video) {
    s.type = MediaType::kVideo;
    s.video.codec = payload.codec;
    s.video.codec_name = CodecName(payload.codec);
    s.video.extradata = std::move(config);
    if (payload.codec != VideoCodec::kAV1) {
      AnnexBConverter converter;
      if (!converter.Initialize(s.video)) {
        error_ = "flv: malformed video configuration";
        return false;
      }
      for (const auto& nal : converter.parameter_sets()) {
        std::vector<uint8_t> annex;
        AnnexBConverter::AppendNal(nal.data(), nal.size(), &annex);
        AccessUnitInfo au;
        if (ParseAccessUnit(annex.data(), annex.size(),
                            payload.codec == VideoCodec::kHEVC, &au) &&
            au.has_sps) {
          s.video.width = au.width;
          s.video.height = au.height;
          s.video.sar_num = au.sar_num;
          s.video.sar_den = au.sar_den;
          s.video.color = au.color;
        }
      }
    }
  } else {
    if (config.size() < 2) {
      error_ = "flv: truncated AAC configuration";
      return false;
    }
    s.type = MediaType::kAudio;
    s.audio.codec = AudioCodec::kAAC;
    s.audio.codec_name = "aac";
    s.audio.extradata = std::move(config);
    static constexpr int rates[] = {96000, 88200, 64000, 48000, 44100,
                                    32000, 24000, 22050, 16000, 12000,
                                    11025, 8000,  7350};
    const auto* asc = s.audio.extradata.data();
    const int rate = ((asc[0] & 7) << 1) | (asc[1] >> 7);
    const int channels = (asc[1] >> 3) & 15;
    if (rate >= 13 || channels < 1 || channels > 7) {
      error_ = "flv: unsupported AAC configuration";
      return false;
    }
    s.audio.sample_rate = rates[rate];
    s.audio.channels = channels == 7 ? 8 : channels;
    step = 1024LL * 1000000 / s.audio.sample_rate;
  }
  index = static_cast<int>(info_.streams.size());
  info_.streams.push_back(std::move(s));
  last_dts_.push_back(kNoTimestamp);
  step_.push_back(step);
  return true;
}

void FlvDemuxer::ParseMetadata(const uint8_t* data, size_t size) {
  Amf amf(data, size);
  uint8_t marker;
  std::string name;
  if (!amf.U8(&marker) || marker != 2 || !amf.String(&name) ||
      name != "onMetaData" || !amf.U8(&marker))
    return;
  if (marker == 8) {
    if (!amf.Skip(4)) return;
  } else if (marker != 3) {
    return;
  }
  while (amf.String(&name) && !name.empty()) {
    uint8_t type;
    if (!amf.Peek(&type)) return;
    if (type != 0) {
      if (!amf.SkipValue(8)) return;
      continue;
    }
    double value;
    if (!amf.U8(&type) || !amf.Double(&value)) return;
    // Converting a NaN or a huge double to int is undefined.
    const bool size = value > 0 && value < 65536;
    if (name == "duration" && value > 0 && value < 1e9) {
      metadata_duration_ = value;
    } else if (name == "width" && size) {
      metadata_width_ = static_cast<int>(value);
    } else if (name == "height" && size) {
      metadata_height_ = static_cast<int>(value);
    }
  }
}

void FlvDemuxer::FinishStreams() {
  if (video_ < 0) return;
  VideoConfig& video = info_.streams[static_cast<size_t>(video_)].video;
  if (video.width <= 0 || video.height <= 0) {
    video.width = metadata_width_;
    video.height = metadata_height_;
  }
}

bool FlvDemuxer::ParseIndexed(int64_t offset) {
  // Per stream, the index of its last sample, whose duration the next
  // one sets.
  std::vector<int64_t> previous;
  uint8_t header[11];
  while (offset < source_->size()) {
    if (!Exact(offset, header, 11)) return false;
    const uint32_t size = BE24(header + 1);
    if (BE24(header + 8) != 0 || (header[0] & 0xe0) || size > kMaxTagSize)
      return false;
    const int64_t dts = TagTime(header);
    const int64_t payload = offset + 11;
    uint8_t head[kPayloadHead] = {};
    if (!Exact(payload, head, std::min<size_t>(size, kPayloadHead)))
      return false;
    Payload p;
    if (!Inspect(header[0], head, size, &p)) return false;
    if (p.kind == Payload::kConfig ||
        (p.kind == Payload::kMetadata && size <= kMaxMetadataSize)) {
      std::vector<uint8_t> bytes(size - p.skip);
      if (!Exact(payload + static_cast<int64_t>(p.skip), bytes.data(),
                 bytes.size()))
        return false;
      if (p.kind == Payload::kMetadata) {
        ParseMetadata(bytes.data(), bytes.size());
      } else if (!AddConfig(p, std::move(bytes))) {
        return false;
      }
      previous.resize(info_.streams.size(), -1);
    } else if (p.kind == Payload::kFrame) {
      const int stream = p.type == MediaType::kVideo ? video_ : audio_;
      if (stream >= 0 && size > p.skip) {
        const int64_t prior = previous[static_cast<size_t>(stream)];
        if (prior >= 0) {
          Sample& last = samples_[static_cast<size_t>(prior)];
          if (dts < last.dts) return false;
          last.duration = dts - last.dts;
        }
        previous[static_cast<size_t>(stream)] =
            static_cast<int64_t>(samples_.size());
        if (!Add({stream, payload + static_cast<int64_t>(p.skip),
                  static_cast<uint32_t>(size - p.skip), dts + p.cts, dts,
                  step_[static_cast<size_t>(stream)], p.key}))
          return false;
      }
    }
    offset = payload + size;
    if (!Exact(offset, header, 4) || BE32(header) != size + 11) return false;
    offset += 4;
  }
  return true;
}

FlvDemuxer::Step FlvDemuxer::Need(size_t size) {
  while (buffer_.size() - buffer_head_ < size) {
    if (interrupted_ || aborted_) return Step::kAborted;
    if (buffer_head_ == buffer_.size()) {
      buffer_.clear();
      buffer_head_ = 0;
    } else if (buffer_head_ >= kCompactAt) {
      buffer_.erase(buffer_.begin(),
                    buffer_.begin() + static_cast<ptrdiff_t>(buffer_head_));
      buffer_head_ = 0;
    }
    const size_t have = buffer_.size();
    buffer_.resize(have + kReadSize);
    const int64_t n =
        source_->Read(read_offset_, buffer_.data() + have, kReadSize);
    buffer_.resize(have + static_cast<size_t>(std::clamp<int64_t>(
                              n, 0, static_cast<int64_t>(kReadSize))));
    if (n > 0 && n <= static_cast<int64_t>(kReadSize)) {
      read_offset_ += n;
      continue;
    }
    if (interrupted_ || aborted_) return Step::kAborted;
    if (n == 0 && buffer_head_ == buffer_.size()) return Step::kEnd;
    error_ = n < 0 ? source_->error()
                   : TagError(ErrorKind::kEndedEarly,
                              "flv: stream ended inside a tag");
    return Step::kError;
  }
  return Step::kOk;
}

FlvDemuxer::Step FlvDemuxer::ReadHeader() {
  Step step = Need(9);
  if (step != Step::kOk) {
    if (step == Step::kEnd) error_ = "flv: empty response";
    return step == Step::kEnd ? Step::kError : step;
  }
  const uint8_t* h = buffer_.data() + buffer_head_;
  const uint32_t offset = BE32(h + 5);
  if (!Detect(h, 9) || offset < 9 || offset > kCompactAt) {
    error_ = "flv: not an FLV stream";
    return Step::kError;
  }
  flags_ = h[4];
  step = Need(offset + 4);
  if (step != Step::kOk) return step == Step::kEnd ? Step::kError : step;
  if (BE32(buffer_.data() + buffer_head_ + offset) != 0) {
    error_ = "flv: not an FLV stream";
    return Step::kError;
  }
  buffer_head_ += offset + 4;
  return Step::kOk;
}

int64_t FlvDemuxer::Rebase(int stream, int64_t time, bool jump) {
  int64_t dts = time + timeline_offset_;
  if (newest_ != kNoTimestamp && (jump || dts < newest_ - kMaxBackwardJump ||
                                  dts > newest_ + kMaxForwardJump)) {
    const int64_t continued = newest_ + step_[static_cast<size_t>(stream)];
    LOGI("flv: timestamps jump from %lld to %lld ms; continuing at %lld ms",
         static_cast<long long>(newest_ / 1000),
         static_cast<long long>(dts / 1000),
         static_cast<long long>(continued / 1000));
    timeline_offset_ = continued - time;
    dts = continued;
  }
  newest_ = newest_ == kNoTimestamp ? dts : std::max(newest_, dts);
  return dts;
}

FlvDemuxer::Step FlvDemuxer::ReadTag() {
  Step step = Need(11);
  if (step != Step::kOk) return step;
  const uint8_t* h = buffer_.data() + buffer_head_;
  const uint32_t size = BE24(h + 1);
  if (BE24(h + 8) != 0 || (h[0] & 0xe0) || size > kMaxTagSize) {
    error_ = "flv: corrupt tag";
    return Step::kError;
  }
  step = Need(size_t{11} + size + 4);
  if (step != Step::kOk) return step == Step::kEnd ? Step::kError : step;
  h = buffer_.data() + buffer_head_;
  const uint8_t* payload = h + 11;
  if (BE32(payload + size) != size + 11) {
    error_ = "flv: corrupt tag";
    return Step::kError;
  }
  uint8_t head[kPayloadHead] = {};
  memcpy(head, payload, std::min<size_t>(size, kPayloadHead));
  Payload p;
  if (!Inspect(h[0], head, size, &p)) return Step::kInvalid;
  if (p.kind == Payload::kConfig) {
    if (!AddConfig(p, std::vector<uint8_t>(payload + p.skip, payload + size)))
      return Step::kInvalid;
  } else if (p.kind == Payload::kMetadata && size <= kMaxMetadataSize) {
    ParseMetadata(payload, size);
  } else if (p.kind == Payload::kFrame) {
    const int stream = p.type == MediaType::kVideo ? video_ : audio_;
    // Frames before their configuration cannot be decoded.
    const auto index = static_cast<size_t>(stream);
    const int64_t time = TagTime(h);
    const int64_t on = time + timeline_offset_;
    bool jump = false;
    if (stream >= 0 && resumed_ && newest_ != kNoTimestamp) {
      resumed_ = false;
      // Up to kMaxForwardJump ahead it goes on as it is (Rebase()).
      if (on <= newest_ && on >= newest_ - kMaxResend) {
        LOGI("flv: the stream goes on from %lld ms, with %lld ms sent again",
             static_cast<long long>(on / 1000),
             static_cast<long long>((newest_ - on) / 1000));
        resending_.assign(info_.streams.size(), true);
      } else if (on < newest_) {
        jump = true;
      }
    }
    bool again = false;
    if (stream >= 0 && index < resending_.size() && resending_[index]) {
      again = on <= last_dts_[index] && on >= newest_ - kMaxResend;
      resending_[index] = again;
    }
    if (stream >= 0 && size > p.skip && !again) {
      const int64_t dts = Rebase(stream, time, jump);
      if (p.type == MediaType::kVideo && last_dts_[index] != kNoTimestamp &&
          dts > last_dts_[index] && dts - last_dts_[index] <= 1000000)
        step_[index] = dts - last_dts_[index];
      last_dts_[index] = dts;
      EncodedPacket packet;
      packet.stream_index = stream;
      packet.dts = dts;
      packet.pts = dts + p.cts;
      packet.duration = step_[index];
      packet.keyframe = p.key;
      packet.buffer = PacketBuffer::Copy(payload + p.skip, size - p.skip);
      pending_.push_back(std::move(packet));
      reconnects_ = 0;
    }
  }
  buffer_head_ += size_t{11} + size + 4;
  return Step::kOk;
}

bool FlvDemuxer::OpenStream() {
  Step step = ReadHeader();
  while (step == Step::kOk) {
    const bool want_video = (flags_ & 1) && video_ < 0;
    const bool want_audio = (flags_ & 4) && audio_ < 0;
    // At least one frame, for the time the stream starts at.
    if (!info_.streams.empty() && !pending_.empty() &&
        ((!want_video && !want_audio) || pending_.size() >= kMaxOpenFrames ||
         newest_ - pending_.front().dts >= kConfigWait))
      break;
    if (pending_.size() >= kMaxOpenFrames) {
      error_ = "flv: no audio or video configuration";
      return false;
    }
    step = ReadTag();
  }
  if (step == Step::kEnd) {
    ended_ = true;
  } else if (step != Step::kOk) {
    return false;
  }
  if (info_.streams.empty()) {
    if (error_.empty()) error_ = "flv: no audio or video configuration";
    return false;
  }
  FinishStreams();
  info_.realtime = !(metadata_duration_ > 0);
  if (!pending_.empty()) {
    info_.start_time = pending_.front().pts;
    for (const EncodedPacket& packet : pending_)
      info_.start_time = std::min(info_.start_time, packet.pts);
  }
  LOGI("flv: reading as it arrives%s",
       info_.realtime ? ", as a live stream" : "");
  return true;
}

FlvDemuxer::Step FlvDemuxer::Reconnect() {
  const size_t attempts = after_end_ ? 1 : std::size(kReconnectDelays);
  while (reconnects_ < attempts) {
    const int delay = kReconnectDelays[reconnects_++];
    {
      std::unique_lock lock(wait_mutex_);
      if (wait_.wait_for(lock, std::chrono::seconds(delay),
                         [this] { return interrupted_ || aborted_; })) {
        reconnects_--;
        return Step::kAborted;
      }
    }
    std::string why;
    std::unique_ptr<DataSource> source = reopener_(url_, &why);
    if (source) {
      {
        std::lock_guard lock(source_mutex_);
        std::swap(source_, source);
        if (interrupted_ || aborted_) source_->Interrupt();
      }
      // The old source closes outside the lock Interrupt() takes.
      source.reset();
      buffer_.clear();
      buffer_head_ = 0;
      read_offset_ = 0;
      const Step step = ReadHeader();
      if (step == Step::kAborted) {
        reconnects_--;
        return step;
      }
      if (step == Step::kOk) {
        LOGI("flv: reconnected");
        broken_ = false;
        resumed_ = true;
        return Step::kOk;
      }
      why = error_;
    }
    LOGW("flv: reconnect %zu of %zu failed: %s", reconnects_, attempts,
         why.c_str());
    error_ = why;
  }
  return after_end_ ? Step::kEnd : Step::kError;
}

DemuxerReadResult FlvDemuxer::Read(EncodedPacket* packet) {
  if (!streaming_) return IndexedDemuxer::Read(packet);
  while (true) {
    if (interrupted_ || aborted_) return DemuxerReadResult::kAborted;
    while (!pending_.empty()) {
      EncodedPacket next = std::move(pending_.front());
      pending_.pop_front();
      if (enabled_[static_cast<size_t>(next.stream_index)]) {
        *packet = std::move(next);
        return DemuxerReadResult::kOk;
      }
    }
    if (ended_) return DemuxerReadResult::kEndOfStream;
    const bool reconnecting = broken_;
    const Step step = reconnecting ? Reconnect() : ReadTag();
    if (step == Step::kAborted) return DemuxerReadResult::kAborted;
    if (step == Step::kInvalid) return DemuxerReadResult::kError;
    if (step == Step::kOk) continue;
    // Live: what follows is wherever the stream is by now. Anything else
    // would start over.
    if (reconnecting || !info_.realtime || !reopener_) {
      if (step == Step::kError) return DemuxerReadResult::kError;
      ended_ = true;
      continue;
    }
    after_end_ = step == Step::kEnd;
    if (after_end_) {
      LOGI("flv: the stream ended; reconnecting in case it broke off");
    } else {
      LOGW("flv: %s; reconnecting", error_.c_str());
    }
    broken_ = true;
  }
}

void FlvDemuxer::Interrupt() {
  IndexedDemuxer::Interrupt();
  {
    std::lock_guard lock(wait_mutex_);
  }
  wait_.notify_all();
}

}  // namespace lychee
