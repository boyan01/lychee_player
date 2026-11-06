#include "backends/audio/wav_demuxer.h"
namespace lychee {
bool WavDemuxer::Parse() {
  uint8_t header[12];
  if (!Exact(0, header, 12) || !Detect(header, 12)) return false;
  const int64_t riff_end = int64_t(LE32(header + 4)) + 8;
  bool format = false;
  for (int64_t at = 12; at + 8 <= riff_end;) {
    uint8_t chunk[8];
    if (!Exact(at, chunk, 8)) return false;
    const uint32_t size = LE32(chunk + 4);
    const int64_t body = at + 8;
    if (size > static_cast<uint64_t>(riff_end - body)) return false;
    if (!memcmp(chunk, "fmt ", 4)) {
      uint8_t fmt[40];
      const size_t count = std::min<size_t>(size, sizeof(fmt));
      if (count < 16 || !Exact(body, fmt, count)) return false;
      unsigned tag = LE16(fmt);
      const int channels = LE16(fmt + 2);
      rate_ = static_cast<int>(LE32(fmt + 4));
      block_ = LE16(fmt + 12);
      const int bits = LE16(fmt + 14);
      if (tag == 0xfffe) {
        static constexpr uint8_t kTail[] = {0, 0,    0x10, 0,    0x80, 0,
                                            0, 0xaa, 0,    0x38, 0x9b, 0x71};
        if (count < 40 || LE16(fmt + 16) < 22 ||
            memcmp(fmt + 28, kTail, 12) != 0)
          return false;
        const uint32_t mask = LE32(fmt + 20);
        // The common mixer uses WAVE's FL FR FC LFE BL BR SL SR order.
        // Refuse other speaker placements instead of silently remixing them.
        static constexpr uint32_t masks[] = {0,    4,    3,     7,    0xf,
                                             0x1f, 0x3f, 0x23f, 0x63f};
        if (mask && (channels > 8 || mask != masks[channels])) return false;
        tag = LE32(fmt + 24);
        const unsigned valid = LE16(fmt + 18);
        if (valid == 0 || valid > static_cast<unsigned>(bits)) return false;
      }
      if (channels < 1 || channels > 32 || rate_ < 1 || rate_ > 768000 ||
          (tag != 1 && tag != 3) ||
          (bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64) ||
          (tag == 1 && bits > 32) || (tag == 3 && bits != 32 && bits != 64) ||
          block_ != channels * (bits / 8))
        return false;
      stream_.audio = {AudioCodec::kPCM,
                       std::string("pcm_") +
                           (tag == 3    ? "f"
                            : bits == 8 ? "u"
                                        : "s") +
                           std::to_string(bits) + (bits == 8 ? "" : "le"),
                       rate_, channels};
      // An internal, portable PCM format: width in bits, floating point,
      // unsigned. No platform-specific WAVEFORMATEX crosses the decoder.
      stream_.audio.extradata = {static_cast<uint8_t>(bits),
                                 static_cast<uint8_t>(tag == 3),
                                 static_cast<uint8_t>(bits == 8 && tag == 1)};
      format = true;
    } else if (!memcmp(chunk, "data", 4)) {
      if (!format || size % block_) return false;
      start_ = position_ = body;
      end_ = body + size;
      if (file_size_ >= 0 && end_ > file_size_) return false;
      media_info_.duration = (size / block_) * kMicrosPerSecond / rate_;
      return true;
    }
    at = body + size + (size & 1);
  }
  return false;
}
DemuxerReadResult WavDemuxer::Read(EncodedPacket* packet) {
  if (Cancelled()) return DemuxerReadResult::kAborted;
  if (!enabled_ || position_ >= end_) return DemuxerReadResult::kEndOfStream;
  const size_t size = static_cast<size_t>(
      std::min<int64_t>(end_ - position_, int64_t(1024) * block_));
  const int64_t sample = (position_ - start_) / block_;
  const int64_t pts = sample * kMicrosPerSecond / rate_;
  const int64_t end = (sample + size / block_) * kMicrosPerSecond / rate_;
  if (!Packet(position_, size, pts, end - pts, packet)) return Failure();
  position_ += static_cast<int64_t>(size);
  return DemuxerReadResult::kOk;
}
bool WavDemuxer::Seek(int64_t time) {
  if (!BeginSeek()) return false;
  const int64_t clamped = std::clamp<int64_t>(time, 0, media_info_.duration);
  position_ = start_ + std::min((end_ - start_) / block_,
                                clamped * rate_ / kMicrosPerSecond) *
                           block_;
  return true;
}
}  // namespace lychee
