#include "backends/audio/mp3_demuxer.h"
#include <cmath>
#include <limits>
namespace lychee {
bool Mp3Demuxer::Detect(const uint8_t* data, size_t size) {
  AdtsHeader header;
  return (size >= 3 && memcmp(data, "ID3", 3) == 0) ||
         ParseMp3Header(data, size, &header);
}
bool Mp3Demuxer::Find(int64_t* offset, AdtsHeader* header) {
  // Bounded resync: malformed bytes must not make opening scan the file.
  uint8_t bytes[4];
  for (int i = 0; i < 65536 && !Cancelled(); ++i, ++*offset) {
    if (!Exact(*offset, bytes, sizeof(bytes))) return false;
    if (ParseMp3Header(bytes, sizeof(bytes), header) &&
        (rate_ == 0 || header->sample_rate == rate_))
      return true;
  }
  return false;
}
bool Mp3Demuxer::Parse() {
  uint8_t tag[10];
  if (!Exact(0, tag, sizeof(tag))) return false;
  if (memcmp(tag, "ID3", 3) == 0) {
    if ((tag[6] | tag[7] | tag[8] | tag[9]) & 0x80) return false;
    first_ = 10 + (int64_t(tag[6]) << 21) + (int64_t(tag[7]) << 14) +
             (int64_t(tag[8]) << 7) + tag[9];
    if (tag[3] == 4 && (tag[5] & 0x10)) first_ += 10;
  }
  AdtsHeader header;
  if (!Find(&first_, &header)) return false;
  rate_ = header.sample_rate;
  stream_.audio = {AudioCodec::kMP3, "mp3", rate_, header.channels};
  audio_bytes_ = file_size_ >= 0 ? file_size_ - first_ : 0;
  std::vector<uint8_t> frame(header.frame_length);
  if (!Exact(first_, frame.data(), frame.size())) return false;
  // ISO side information precedes the Xing/Info header. MPEG-2/2.5
  // has a shorter side-info block; a protected frame also has a CRC.
  const bool mpeg1 = ((frame[1] >> 3) & 3) == 3;
  const size_t side = mpeg1 ? (header.channels == 1 ? 17 : 32)
                            : (header.channels == 1 ? 9 : 17);
  const size_t xing = 4 + ((frame[1] & 1) ? 0 : 2) + side;
  bool info_frame = false;
  int64_t encoder_delay = 0;
  int64_t encoder_padding = 0;
  if (xing + 8 <= frame.size() &&
      (memcmp(frame.data() + xing, "Xing", 4) == 0 ||
       memcmp(frame.data() + xing, "Info", 4) == 0)) {
    info_frame = true;
    const uint32_t flags = BE32(frame.data() + xing + 4);
    size_t at = xing + 8;
    if ((flags & 1) && at + 4 <= frame.size()) {
      total_samples_ = int64_t(BE32(frame.data() + at)) * header.samples;
      at += 4;
    }
    if ((flags & 2) && at + 4 <= frame.size()) {
      const uint32_t declared = BE32(frame.data() + at);
      if (declared > 0 && (audio_bytes_ == 0 || declared <= audio_bytes_))
        audio_bytes_ = declared;
      at += 4;
    }
    if ((flags & 4) && at + toc_.size() <= frame.size()) {
      std::copy_n(frame.data() + at, toc_.size(), toc_.begin());
      has_toc_ = std::is_sorted(toc_.begin(), toc_.end());
      at += toc_.size();
    }
    if (flags & 8) at += 4;
    // LAME's gapless extension is also written by FFmpeg (Lavc). The
    // Layer III decoder adds 529 samples of synthesis delay; containers
    // count that in the start and subtract it from the encoder padding.
    if (at + 24 <= frame.size() && (!memcmp(frame.data() + at, "LAME", 4) ||
                                    !memcmp(frame.data() + at, "Lavc", 4) ||
                                    !memcmp(frame.data() + at, "Lavf", 4))) {
      encoder_delay = (frame[at + 21] << 4) | (frame[at + 22] >> 4);
      encoder_padding = ((frame[at + 22] & 15) << 8) | frame[at + 23];
    }
  }
  // VBRI uses a fixed offset from the MPEG header, independent of side
  // information. Its entries are byte increments over equal frame spans.
  const size_t v = 36;
  if (!info_frame && v + 26 <= frame.size() &&
      memcmp(frame.data() + v, "VBRI", 4) == 0) {
    total_samples_ = int64_t(BE32(frame.data() + v + 14)) * header.samples;
    const unsigned count = (frame[v + 18] << 8) | frame[v + 19];
    const unsigned scale = (frame[v + 20] << 8) | frame[v + 21];
    const unsigned width = (frame[v + 22] << 8) | frame[v + 23];
    const unsigned span = (frame[v + 24] << 8) | frame[v + 25];
    if (width >= 1 && width <= 4 && count <= (frame.size() - v - 26) / width &&
        span && scale) {
      int64_t offset = first_;
      for (unsigned i = 0; i < count; ++i) {
        vbri_.push_back({int64_t(i) * span * header.samples, offset});
        uint32_t bytes = 0;
        for (unsigned j = 0; j < width; ++j)
          bytes = (bytes << 8) | frame[v + 26 + size_t{i} * width + j];
        if (uint64_t(bytes) * scale >
            uint64_t(std::numeric_limits<int64_t>::max() - offset))
          return false;
        offset += int64_t(bytes) * scale;
      }
    }
  }
  if (total_samples_ == 0 && audio_bytes_ > 0) {
    const double samples =
        double(audio_bytes_) / header.frame_length * header.samples;
    if (samples > double(INT64_MAX / kMicrosPerSecond)) return false;
    total_samples_ = static_cast<int64_t>(samples);
  }
  if (encoder_delay + encoder_padding < total_samples_ &&
      encoder_padding >= 529) {
    media_info_.start_time = (encoder_delay + 529) * kMicrosPerSecond / rate_;
    media_info_.duration = (total_samples_ - encoder_delay - encoder_padding) *
                           kMicrosPerSecond / rate_;
  } else if (total_samples_ > 0) {
    media_info_.duration = total_samples_ * kMicrosPerSecond / rate_;
  }
  // Xing's synthetic frame is metadata, not the first audio frame.
  audio_start_ = position_ = first_ + (info_frame ? header.frame_length : 0);
  return true;
}
DemuxerReadResult Mp3Demuxer::Read(EncodedPacket* packet) {
  if (Cancelled()) return DemuxerReadResult::kAborted;
  if (!enabled_) return DemuxerReadResult::kEndOfStream;
  AdtsHeader header;
  const int64_t before = position_;
  if (!Find(&position_, &header)) {
    if (Cancelled()) return DemuxerReadResult::kAborted;
    if (file_size_ >= 0 && before >= file_size_ - 128)
      return DemuxerReadResult::kEndOfStream;
    if (file_size_ < 0 && error_.empty())
      return DemuxerReadResult::kEndOfStream;
    return Failure();
  }
  const int64_t pts = samples_ * kMicrosPerSecond / rate_;
  const int64_t end = (samples_ + header.samples) * kMicrosPerSecond / rate_;
  if (!Packet(position_, header.frame_length, pts, end - pts, packet))
    return Failure();
  position_ += static_cast<int64_t>(header.frame_length);
  samples_ += header.samples;
  return DemuxerReadResult::kOk;
}
bool Mp3Demuxer::Seek(int64_t time) {
  if (!BeginSeek()) return false;
  const int64_t target =
      std::max<int64_t>(0, time - media_info_.start_time - 100000);
  const double fraction =
      media_info_.duration > 0
          ? std::clamp(double(target) / media_info_.duration, 0.0, 1.0)
          : 0;
  double byte_fraction = fraction;
  if (has_toc_) {
    const double percent = fraction * 100;
    const int index = std::min(99, static_cast<int>(percent));
    const double lo = toc_[index];
    const double hi = index == 99 ? 256 : toc_[index + 1];
    byte_fraction = (lo + (hi - lo) * (percent - index)) / 256;
  }
  position_ = first_ + static_cast<int64_t>(byte_fraction * audio_bytes_);
  samples_ = static_cast<int64_t>(fraction * total_samples_);
  if (!vbri_.empty()) {
    const auto it = std::upper_bound(vbri_.begin(), vbri_.end(), samples_,
                                     [](int64_t sample, const Point& point) {
                                       return sample < point.sample;
                                     });
    const auto& point = it == vbri_.begin() ? vbri_.front() : *std::prev(it);
    position_ = point.offset;
    samples_ = point.sample;
  }
  if (target == 0) {
    samples_ = 0;
    position_ = audio_start_;
  }
  return true;
}
}  // namespace lychee
