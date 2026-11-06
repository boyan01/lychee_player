#include "backends/audio/flac_demuxer.h"
namespace lychee {
bool FlacDemuxer::Parse() {
  uint8_t bytes[4];
  if (!Exact(0, bytes, 4) || !Detect(bytes, 4)) return false;
  int64_t at = 4;
  bool info = false;
  for (int blocks = 0; blocks < 1024; ++blocks) {
    if (!Exact(at, bytes, 4)) return false;
    const unsigned type = bytes[0] & 0x7f;
    const bool last = bytes[0] & 0x80;
    const size_t size =
        (size_t(bytes[1]) << 16) | (size_t(bytes[2]) << 8) | bytes[3];
    at += 4;
    if (file_size_ >= 0 && size > static_cast<uint64_t>(file_size_ - at))
      return false;
    if (!info && (type != 0 || size != 34)) return false;
    if (type == 0) {
      if (info || size != 34) return false;
      std::vector<uint8_t> metadata(size);
      if (!Exact(at, metadata.data(), size)) return false;
      block_ = (metadata[2] << 8) | metadata[3];
      rate_ = (metadata[10] << 12) | (metadata[11] << 4) | (metadata[12] >> 4);
      const int channels = ((metadata[12] >> 1) & 7) + 1;
      const int bits = (((metadata[12] & 1) << 4) | (metadata[13] >> 4)) + 1;
      total_ = (int64_t(metadata[13] & 15) << 32) | BE32(metadata.data() + 14);
      if (rate_ <= 0 || block_ < 16 || bits < 4 || bits > 32) return false;
      stream_.audio = {AudioCodec::kFLAC, "flac", rate_, channels,
                       std::move(metadata)};
      info = true;
    } else if (type == 3) {
      if (size % 18) return false;
      for (size_t p = 0; p < size; p += 18) {
        uint8_t point[18];
        if (!Exact(at + static_cast<int64_t>(p), point, 18)) return false;
        if (point[0] == 0xff) continue;
        const uint64_t sample = (uint64_t(BE32(point)) << 32) | BE32(point + 4);
        const uint64_t offset =
            (uint64_t(BE32(point + 8)) << 32) | BE32(point + 12);
        if (sample > uint64_t(INT64_MAX) || offset > uint64_t(INT64_MAX) ||
            (!points_.empty() &&
             sample <= static_cast<uint64_t>(points_.back().sample)))
          return false;
        points_.push_back(
            {static_cast<int64_t>(sample), static_cast<int64_t>(offset)});
      }
    }
    at += static_cast<int64_t>(size);
    if (last) {
      start_ = position_ = at;
      for (auto& point : points_) {
        if (point.offset > INT64_MAX - start_) return false;
        point.offset += start_;
      }
      if (total_ > 0) media_info_.duration = total_ * kMicrosPerSecond / rate_;
      Header h;
      return FrameHeader(start_, &h);
    }
  }
  return false;
}
bool FlacDemuxer::FrameHeader(int64_t at, Header* h) {
  uint8_t p[32];
  const int64_t got = Bytes(at, p, sizeof(p));
  if (got < 6 || p[0] != 0xff || (p[1] & 0xfe) != 0xf8 || (p[3] & 1) ||
      (p[3] >> 4) > 10)
    return false;
  const unsigned code = p[2] >> 4;
  const unsigned sr = p[2] & 15;
  if (!code || sr == 15) return false;
  size_t pos = 4;
  const uint8_t first = p[pos++];
  int continuation = 0;
  uint64_t number = 0;
  if (!(first & 0x80))
    number = first;
  else {
    uint8_t mask = 0x80;
    while (first & mask) {
      ++continuation;
      mask >>= 1;
    }
    if (continuation < 2 || continuation > 7) return false;
    number = first & (mask - 1);
    --continuation;
    for (int i = 0; i < continuation; ++i) {
      if (pos >= static_cast<size_t>(got) || (p[pos] & 0xc0) != 0x80)
        return false;
      number = (number << 6) | (p[pos++] & 63);
    }
  }
  int block = code == 1                ? 192
              : code >= 2 && code <= 5 ? 576 << (code - 2)
              : code >= 8              ? 256 << (code - 8)
                                       : 0;
  if (code == 6) {
    if (pos >= static_cast<size_t>(got)) return false;
    block = p[pos++] + 1;
  }
  if (code == 7) {
    if (pos + 2 > static_cast<size_t>(got)) return false;
    block = (p[pos] << 8) + p[pos + 1] + 1;
    pos += 2;
  }
  if (sr == 12)
    ++pos;
  else if (sr == 13 || sr == 14)
    pos += 2;
  if (pos >= static_cast<size_t>(got)) return false;
  uint8_t crc = 0;
  for (size_t i = 0; i < pos; ++i) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = static_cast<uint8_t>((crc << 1) ^ ((crc & 0x80) ? 7 : 0));
  }
  if (crc != p[pos] || number > uint64_t(INT64_MAX / std::max(1, block_)))
    return false;
  if (number >
      ((uint64_t(1) << 36) - 1) / ((p[1] & 1) ? 1 : std::max(1, block_)))
    return false;
  h->block = block;
  h->sample = (p[1] & 1) ? static_cast<int64_t>(number)
                         : static_cast<int64_t>(number) * block_;
  return block > 0;
}
bool FlacDemuxer::Find(int64_t* at, Header* h, int64_t limit) {
  uint8_t byte;
  for (; *at < limit && !Cancelled(); ++*at) {
    if (!Exact(*at, &byte, 1)) return false;
    if (byte == 0xff && FrameHeader(*at, h)) return true;
  }
  return false;
}
DemuxerReadResult FlacDemuxer::Read(EncodedPacket* packet) {
  if (Cancelled()) return DemuxerReadResult::kAborted;
  if (!enabled_ || (file_size_ >= 0 && position_ >= file_size_))
    return DemuxerReadResult::kEndOfStream;
  Header h;
  if (!FrameHeader(position_, &h)) return Failure();
  int64_t next = position_ + 2;
  Header following;
  const int64_t limit =
      file_size_ >= 0
          ? std::min(file_size_, position_ + int64_t{16} * 1024 * 1024)
          : position_ + int64_t{16} * 1024 * 1024;
  bool found = false;
  while (Find(&next, &following, limit)) {
    if (following.sample == h.sample + h.block) {
      found = true;
      break;
    }
    ++next;
  }
  if (!found) {
    if (Cancelled()) return DemuxerReadResult::kAborted;
    if (file_size_ < 0 || file_size_ > limit) return Failure();
    next = file_size_;
    if (total_ > 0 && h.sample + h.block < total_) return Failure();
  }
  const int64_t pts = h.sample * kMicrosPerSecond / rate_;
  const int64_t end = (h.sample + h.block) * kMicrosPerSecond / rate_;
  if (!Packet(position_, static_cast<size_t>(next - position_), pts, end - pts,
              packet))
    return Failure();
  position_ = next;
  return DemuxerReadResult::kOk;
}
bool FlacDemuxer::Seek(int64_t time) {
  if (!BeginSeek()) return false;
  const int64_t target =
      std::clamp<int64_t>(time, 0, std::max<int64_t>(0, media_info_.duration)) *
      rate_ / kMicrosPerSecond;
  int64_t lo = start_, hi = file_size_;
  for (const auto& point : points_) {
    if (point.offset < start_ || point.offset >= file_size_) continue;
    if (point.sample <= target)
      lo = point.offset;
    else {
      hi = point.offset;
      break;
    }
  }
  for (int i = 0; i < 48 && hi - lo > 65536; ++i) {
    int64_t at = lo + (hi - lo) / 2;
    Header h;
    if (!Find(&at, &h, std::min(hi, at + 65536))) {
      hi = lo + (hi - lo) / 2;
      continue;
    }
    if (h.sample <= target)
      lo = at;
    else
      hi = lo + (hi - lo) / 2;
  }
  position_ = lo;
  Header h;
  if (!Find(&position_, &h,
            std::min(file_size_, lo + int64_t{16} * 1024 * 1024)))
    return false;
  for (;;) {
    if (h.sample + h.block > target) return true;
    int64_t next = position_ + 2;
    Header following;
    if (!Find(&next, &following,
              std::min(file_size_, next + int64_t{16} * 1024 * 1024)))
      return true;
    if (following.sample > target) return true;
    position_ = next;
    h = following;
  }
}
}  // namespace lychee
