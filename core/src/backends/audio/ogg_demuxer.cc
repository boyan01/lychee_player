#include "backends/audio/ogg_demuxer.h"
#include <bit>
#include <limits>
namespace lychee {
namespace {
class Bits {
 public:
  explicit Bits(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
  uint32_t Read(unsigned count) {
    if (count > 32 || at_ + count > bytes_.size() * 8) {
      valid_ = false;
      return 0;
    }
    uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i, ++at_)
      value |= uint32_t((bytes_[at_ / 8] >> (at_ % 8)) & 1) << i;
    return value;
  }
  bool Skip(uint64_t count) {
    if (count > bytes_.size() * 8 - at_) {
      valid_ = false;
      return false;
    }
    at_ += static_cast<size_t>(count);
    return true;
  }
  bool valid() const { return valid_; }

 private:
  const std::vector<uint8_t>& bytes_;
  size_t at_ = 0;
  bool valid_ = true;
};
unsigned ILog(unsigned value) {
  return std::bit_width(value);
}
// Walk the Vorbis I setup header to the mode block flags. Every loop is
// bounded by the packet's bytes, including ordered/sparse codebooks.
bool VorbisModes(const std::vector<uint8_t>& setup,
                 int channels,
                 std::vector<bool>* modes) {
  if (setup.size() < 8) return false;
  Bits b(setup);
  b.Skip(uint64_t{7} * 8);
  const unsigned books = b.Read(8) + 1;
  for (unsigned book = 0; book < books && b.valid(); ++book) {
    if (b.Read(24) != 0x564342) return false;
    const unsigned dimensions = b.Read(16), entries = b.Read(24);
    if (!dimensions || entries > setup.size() * 8) return false;
    if (b.Read(1)) {
      b.Read(5);
      unsigned current = 0;
      for (unsigned length = 0; current < entries; ++length) {
        if (length > 32) return false;
        const unsigned count = b.Read(ILog(entries - current));
        if (!b.valid() || count > entries - current) return false;
        current += count;
      }
    } else {
      const bool sparse = b.Read(1);
      for (unsigned i = 0; i < entries && b.valid(); ++i)
        if (!sparse || b.Read(1)) b.Read(5);
    }
    const unsigned lookup = b.Read(4);
    if (lookup > 2) return false;
    if (lookup) {
      b.Skip(64);
      const unsigned bits = b.Read(4) + 1;
      b.Read(1);
      uint64_t values = uint64_t(entries) * dimensions;
      if (lookup == 1) {
        unsigned lo = 0, hi = entries;
        while (lo < hi) {
          const unsigned mid = lo + (hi - lo + 1) / 2;
          uint64_t power = 1;
          for (unsigned j = 0; j < dimensions && power <= entries; ++j) {
            power *= mid;
            if (mid == 0) break;
          }
          if (power <= entries)
            lo = mid;
          else
            hi = mid - 1;
        }
        values = lo;
      }
      if (!b.Skip(values * bits)) return false;
    }
  }
  const unsigned times = b.Read(6) + 1;
  for (unsigned i = 0; i < times; ++i)
    if (b.Read(16)) return false;
  const unsigned floors = b.Read(6) + 1;
  for (unsigned i = 0; i < floors && b.valid(); ++i) {
    const unsigned type = b.Read(16);
    if (type == 0) {
      b.Skip(8 + 16 + 16 + 6 + 8);
      const unsigned books0 = b.Read(4) + 1;
      b.Skip(uint64_t{books0} * 8);
    } else if (type == 1) {
      const unsigned partitions = b.Read(5);
      std::vector<unsigned> classes(partitions);
      int maximum = -1;
      for (auto& c : classes) {
        c = b.Read(4);
        maximum = std::max(maximum, static_cast<int>(c));
      }
      std::vector<unsigned> dimensions(static_cast<size_t>(maximum + 1));
      for (int c = 0; c <= maximum; ++c) {
        dimensions[c] = b.Read(3) + 1;
        const unsigned subs = b.Read(2);
        if (subs) b.Read(8);
        b.Skip((uint64_t{1} << subs) * 8);
      }
      b.Read(2);
      const unsigned range = b.Read(4);
      for (unsigned c : classes)
        b.Skip(uint64_t{dimensions[c]} * range);
    } else
      return false;
  }
  const unsigned residues = b.Read(6) + 1;
  for (unsigned i = 0; i < residues && b.valid(); ++i) {
    if (b.Read(16) > 2) return false;
    b.Skip(72);
    const unsigned classifications = b.Read(6) + 1;
    b.Read(8);
    std::vector<unsigned> cascades(classifications);
    for (auto& cascade : cascades) {
      const unsigned low = b.Read(3);
      cascade = (b.Read(1) ? b.Read(5) << 3 : 0) | low;
    }
    for (unsigned cascade : cascades)
      b.Skip(static_cast<uint64_t>(std::popcount(cascade)) * 8);
  }
  const unsigned mappings = b.Read(6) + 1;
  for (unsigned i = 0; i < mappings && b.valid(); ++i) {
    if (b.Read(16)) return false;
    const unsigned submaps = b.Read(1) ? b.Read(4) + 1 : 1;
    if (b.Read(1)) {
      const unsigned steps = b.Read(8) + 1;
      for (unsigned s = 0; s < steps; ++s) {
        const unsigned a = b.Read(ILog(channels - 1)),
                       c = b.Read(ILog(channels - 1));
        if (a == c || a >= static_cast<unsigned>(channels) ||
            c >= static_cast<unsigned>(channels))
          return false;
      }
    }
    if (b.Read(2)) return false;
    if (submaps > 1)
      for (int c = 0; c < channels; ++c)
        if (b.Read(4) >= submaps) return false;
    b.Skip(uint64_t{submaps} * 24);
  }
  const unsigned count = b.Read(6) + 1;
  modes->clear();
  for (unsigned i = 0; i < count; ++i) {
    modes->push_back(b.Read(1) != 0);
    if (b.Read(16) || b.Read(16) || b.Read(8) >= mappings) return false;
  }
  return b.Read(1) == 1 && b.valid();
}
int OpusSamples(const std::vector<uint8_t>& p) {
  if (p.empty()) return 0;
  const unsigned config = p[0] >> 3;
  static constexpr int silk[] = {480, 960, 1920, 2880},
                       celt[] = {120, 240, 480, 960};
  const int frame = config < 12   ? silk[config & 3]
                    : config < 16 ? ((config & 1) ? 960 : 480)
                                  : celt[config & 3];
  const int count = (p[0] & 3) == 0   ? 1
                    : (p[0] & 3) == 3 ? (p.size() > 1 ? p[1] & 63 : 0)
                                      : 2;
  return frame * count <= 5760 ? frame * count : 0;
}
}  // namespace
bool OggDemuxer::PageAt(int64_t at, Page* page) {
  uint8_t h[27];
  if (!Exact(at, h, sizeof(h)) || !Detect(h, sizeof(h)) || h[4] != 0 ||
      (h[5] & ~7))
    return false;
  const unsigned count = h[26];
  std::vector<uint8_t> laces(count);
  if (!Exact(at + 27, laces.data(), laces.size())) return false;
  size_t size = 0;
  for (uint8_t n : laces)
    size += n;
  std::vector<uint8_t> bytes(27 + count + size);
  if (!Exact(at, bytes.data(), bytes.size())) return false;
  const uint32_t expected = LE32(bytes.data() + 22);
  std::fill_n(bytes.data() + 22, 4, uint8_t{0});
  uint32_t crc = 0;
  for (uint8_t byte : bytes) {
    crc ^= uint32_t(byte) << 24;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
  }
  if (crc != expected) return false;
  page->offset = at;
  page->end = at + static_cast<int64_t>(bytes.size());
  page->flags = h[5];
  const uint64_t granule =
      uint64_t(LE32(h + 6)) | (uint64_t(LE32(h + 10)) << 32);
  if (granule != UINT64_MAX && granule > uint64_t(INT64_MAX / kMicrosPerSecond))
    return false;
  page->granule = granule == UINT64_MAX ? -1 : static_cast<int64_t>(granule);
  page->serial = LE32(h + 14);
  page->sequence = LE32(h + 18);
  page->laces = std::move(laces);
  page->bytes.assign(bytes.begin() + 27 + count, bytes.end());
  return true;
}
bool OggDemuxer::FindPage(int64_t* at, Page* page, int64_t limit) {
  uint8_t p[4];
  for (; *at < limit && !Cancelled(); ++*at) {
    if (!Exact(*at, p, 4)) return false;
    if (Detect(p, 4) && PageAt(*at, page) &&
        (!have_serial_ || page->serial == serial_))
      return true;
  }
  return false;
}
bool OggDemuxer::Header(const std::vector<uint8_t>& p) {
  if (headers_.empty()) {
    if (p.size() >= 19 && !memcmp(p.data(), "OpusHead", 8) && p[8] <= 15) {
      const int channels = p[9];
      if (!channels || channels > 32) return false;
      rate_ = 48000;
      preskip_ = LE16(p.data() + 10);
      header_count_ = 2;
      stream_.audio = {AudioCodec::kOpus, "opus", rate_, channels, p};
    } else if (p.size() >= 30 && p[0] == 1 &&
               memcmp(p.data() + 1, "vorbis", 6) == 0) {
      rate_ = static_cast<int>(LE32(p.data() + 12));
      const int channels = p[11];
      const int small = p[28] & 15, large = p[28] >> 4;
      if (rate_ < 1 || rate_ > 768000 || !channels || channels > 32 ||
          small < 6 || large > 13 || small > large || !(p[29] & 1))
        return false;
      small_block_ = 1 << small;
      large_block_ = 1 << large;
      header_count_ = 3;
      stream_.audio = {AudioCodec::kVorbis, "vorbis", rate_, channels};
    } else if (p.size() >= 51 && p[0] == 0x7f &&
               !memcmp(p.data() + 1, "FLAC", 4) && p[5] == 1 && p[6] == 0 &&
               !memcmp(p.data() + 9, "fLaC", 4)) {
      const uint8_t* info = p.data() + 17;
      rate_ = (info[10] << 12) | (info[11] << 4) | (info[12] >> 4);
      if (!rate_) return false;
      const int channels = ((info[12] >> 1) & 7) + 1;
      header_count_ = 1 + ((p[7] << 8) | p[8]);
      if (header_count_ > 1024) return false;
      stream_.audio = {AudioCodec::kFLAC, "flac", rate_, channels,
                       std::vector<uint8_t>(info, info + 34)};
    } else
      return false;
  } else if (stream_.audio.codec == AudioCodec::kOpus &&
             (p.size() < 8 || memcmp(p.data(), "OpusTags", 8) != 0))
    return false;
  else if (stream_.audio.codec == AudioCodec::kVorbis) {
    const uint8_t type = headers_.size() == 1 ? 3 : 5;
    if (p.size() < 7 || p[0] != type || memcmp(p.data() + 1, "vorbis", 6) != 0)
      return false;
    if (type == 5) {
      if (!VorbisModes(p, stream_.audio.channels, &modes_)) return false;
      // Xiph lacing of the three Vorbis headers, also used by Matroska.
      std::vector<uint8_t> extra = {2};
      for (size_t i = 0; i < 2; ++i) {
        size_t n = headers_[i].size();
        while (n >= 255) {
          extra.push_back(255);
          n -= 255;
        }
        extra.push_back(static_cast<uint8_t>(n));
      }
      for (const auto& h : headers_)
        extra.insert(extra.end(), h.begin(), h.end());
      extra.insert(extra.end(), p.begin(), p.end());
      stream_.audio.extradata = std::move(extra);
    }
  }
  size_t header_bytes = p.size();
  for (const auto& header : headers_)
    header_bytes += header.size();
  if (header_bytes > size_t{16} * 1024 * 1024) return false;
  headers_.push_back(p);
  return true;
}
int OggDemuxer::PacketSamples(const std::vector<uint8_t>& p) {
  if (stream_.audio.codec == AudioCodec::kOpus) return OpusSamples(p);
  if (stream_.audio.codec == AudioCodec::kVorbis) {
    Bits b(p);
    if (b.Read(1) || modes_.empty()) return -1;
    const unsigned mode =
        b.Read(ILog(static_cast<unsigned>(modes_.size() - 1)));
    if (!b.valid() || mode >= modes_.size()) return -1;
    const int block = modes_[mode] ? large_block_ : small_block_;
    const int samples = previous_block_ ? (previous_block_ + block) / 4 : 0;
    previous_block_ = block;
    return samples;
  }
  if (p.size() < 6 || p[0] != 0xff || (p[1] & 0xfe) != 0xf8) return -1;
  const unsigned code = p[2] >> 4;
  if (code == 1) return 192;
  if (code >= 2 && code <= 5) return 576 << (code - 2);
  if (code >= 8) return 256 << (code - 8);
  size_t at = 4;
  if (p[at] & 0x80) {
    uint8_t mask = 0x80;
    unsigned n = 0;
    while (p[at] & mask) {
      ++n;
      mask >>= 1;
    }
    if (n < 2 || n > 7) return -1;
    at += n;
  } else
    ++at;
  if (code == 6 && at < p.size()) return p[at] + 1;
  if (code == 7 && at + 1 < p.size()) return (p[at] << 8) + p[at + 1] + 1;
  return -1;
}
bool OggDemuxer::Fill() {
  for (int pages = 0; pages < 4096 && !Cancelled(); ++pages) {
    Page page;
    if (file_size_ >= 0 && position_ == file_size_) {
      ended_ = true;
      return true;
    }
    if (!PageAt(position_, &page)) {
      if (file_size_ == position_ && partial_.empty() && !Cancelled() &&
          error_.empty()) {
        ended_ = true;
        return true;
      }
      return false;
    }
    position_ = page.end;
    if (!have_serial_) {
      if (!(page.flags & 2)) return false;
      serial_ = page.serial;
      have_serial_ = true;
    }
    if (page.serial != serial_) continue;
    if (have_sequence_ && page.sequence != sequence_ + 1) return false;
    sequence_ = page.sequence;
    have_sequence_ = true;
    if ((page.flags & 1) == 0 && !partial_.empty()) return false;
    if ((page.flags & 1) && partial_.empty()) discard_continued_ = true;
    std::vector<std::vector<uint8_t>> packets;
    size_t at = 0;
    for (uint8_t lace : page.laces) {
      if (!discard_continued_) {
        if (partial_.size() + lace > size_t{16} * 1024 * 1024) return false;
        partial_.insert(partial_.end(), page.bytes.begin() + at,
                        page.bytes.begin() + at + lace);
      }
      at += lace;
      if (lace < 255) {
        if (discard_continued_)
          discard_continued_ = false;
        else if (header_count_ == 0 ||
                 headers_.size() < static_cast<size_t>(header_count_)) {
          if (!Header(partial_)) return false;
          partial_.clear();
        } else {
          packets.push_back(std::move(partial_));
          partial_.clear();
        }
      }
    }
    if (page.granule >= 0) last_granule_ = page.granule;
    if (!packets.empty()) {
      if (audio_start_ < 0) audio_start_ = page.offset;
      std::vector<int> lengths;
      int64_t total = 0;
      for (const auto& p : packets) {
        const int n = PacketSamples(p);
        if (n < 0) return false;
        lengths.push_back(n);
        total += n;
      }
      int64_t sample = next_sample_;
      // A seek begins at an arbitrary page. Its granule anchors the page's
      // completed packets; the final page may trim padding at its end.
      if (sample < 0) sample = std::max<int64_t>(0, page.granule - total);
      for (size_t i = 0; i < packets.size(); ++i) {
        EncodedPacket packet;
        packet.stream_index = 0;
        packet.keyframe = true;
        packet.pts = packet.dts =
            (sample - preskip_) * kMicrosPerSecond / rate_;
        packet.duration = int64_t(lengths[i]) * kMicrosPerSecond / rate_;
        packet.buffer =
            PacketBuffer::Copy(packets[i].data(), packets[i].size());
        ready_.push_back(std::move(packet));
        if (sample > INT64_MAX / kMicrosPerSecond - lengths[i]) return false;
        sample += lengths[i];
      }
      next_sample_ = sample;
    }
    if (page.flags & 4) {
      if (!partial_.empty()) return false;
      ended_ = true;
    }
    if (!ready_.empty() || ended_) return true;
  }
  return false;
}
bool OggDemuxer::Parse() {
  next_sample_ = 0;
  if (!Fill() || rate_ <= 0 ||
      headers_.size() != static_cast<size_t>(header_count_))
    return false;
  if (file_size_ >= 0 && source_->seekable()) {
    int64_t at = std::max<int64_t>(0, file_size_ - 131072);
    Page page;
    int64_t granule = -1;
    while (FindPage(&at, &page, file_size_)) {
      if (page.granule >= 0) granule = page.granule;
      at = page.end;
    }
    if (granule >= preskip_)
      media_info_.duration = (granule - preskip_) * kMicrosPerSecond / rate_;
  }
  return true;
}
DemuxerReadResult OggDemuxer::Read(EncodedPacket* packet) {
  if (Cancelled()) return DemuxerReadResult::kAborted;
  if (!enabled_) return DemuxerReadResult::kEndOfStream;
  if (ready_.empty() && !ended_ && !Fill()) return Failure();
  if (ready_.empty()) return DemuxerReadResult::kEndOfStream;
  *packet = std::move(ready_.front());
  ready_.pop_front();
  return DemuxerReadResult::kOk;
}
bool OggDemuxer::Seek(int64_t time) {
  if (!BeginSeek()) return false;
  const int64_t clamped =
      std::clamp<int64_t>(time, 0, std::max<int64_t>(0, media_info_.duration));
  const int64_t target = std::max<int64_t>(
      0, clamped * rate_ / kMicrosPerSecond + preskip_ - rate_ / 10);
  int64_t lo = 0, hi = file_size_;
  for (int i = 0; i < 48 && hi - lo > 65536; ++i) {
    int64_t at = lo + (hi - lo) / 2;
    const int64_t mid = at;
    Page p;
    if (!FindPage(&at, &p, std::min(file_size_, at + 65536)) || p.granule < 0 ||
        p.granule > target)
      hi = mid;
    else
      lo = at;
  }
  int64_t selected = lo;
  Page page;
  if (!FindPage(&selected, &page, file_size_)) return false;
  // A page's granule belongs to its end. Begin with a page whose end is
  // before the target, retaining at least the codec's seek preroll.
  int64_t at = selected;
  while (FindPage(&at, &page, file_size_)) {
    if (page.granule > target) break;
    selected = at;
    at = page.end;
  }
  if (selected <= audio_start_) selected = 0;
  ready_.clear();
  partial_.clear();
  ended_ = false;
  have_sequence_ = false;
  previous_block_ = 0;
  position_ = selected;
  next_sample_ = selected == 0 ? 0 : -1;
  discard_continued_ = false;
  if (selected == 0) {
    headers_.clear();
    header_count_ = 0;
  }
  return true;
}
}  // namespace lychee
