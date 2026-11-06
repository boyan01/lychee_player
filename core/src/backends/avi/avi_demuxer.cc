#include "backends/avi/avi_demuxer.h"
#include "backends/mp2t/elementary_stream.h"

namespace lychee {
bool AviDemuxer::Format(int track, int64_t offset, uint32_t size) {
  if (track < 0 || size > 65536) return false;
  std::vector<uint8_t> b(size);
  if (!Exact(offset, b.data(), size)) return false;
  auto& t = tracks_[track];
  if (t.index < 0) return true;
  auto& s = info_.streams[t.index];
  if (s.type == MediaType::kVideo) {
    if (size < 40 || LE32(b.data()) < 40 || LE32(b.data()) > size) return false;
    s.video.width = static_cast<int32_t>(LE32(b.data() + 4));
    int32_t height = static_cast<int32_t>(LE32(b.data() + 8));
    if (s.video.width <= 0 || height == INT32_MIN || height == 0) return false;
    s.video.height = height < 0 ? -height : height;
    const char* code = reinterpret_cast<const char*>(b.data() + 16);
    if (!memcmp(code, "H264", 4) || !memcmp(code, "avc1", 4) ||
        !memcmp(code, "X264", 4) || !memcmp(code, "h264", 4)) {
      s.video.codec = VideoCodec::kH264;
      s.video.codec_name = "h264";
    } else if (!memcmp(code, "FMP4", 4) || !memcmp(code, "XVID", 4) ||
               !memcmp(code, "DIVX", 4) || !memcmp(code, "DX50", 4) ||
               !memcmp(code, "MP4V", 4)) {
      s.video.codec = VideoCodec::kMPEG4;
      s.video.codec_name = "mpeg4";
    } else if (!memcmp(code, "mpg2", 4) || !memcmp(code, "MPEG", 4) ||
               !memcmp(code, "MPG2", 4)) {
      s.video.codec = VideoCodec::kMPEG2;
      s.video.codec_name = "mpeg2video";
    } else {
      error_ = "avi: unsupported video codec";
      return false;
    }
    s.video.extradata.assign(b.begin() + LE32(b.data()), b.end());
  } else {
    if (size < 16) return false;
    const unsigned tag = LE16(b.data());
    s.audio.channels = LE16(b.data() + 2);
    const uint32_t rate = LE32(b.data() + 4);
    if (!rate || rate > 768000 || s.audio.channels < 1 || s.audio.channels > 32)
      return false;
    s.audio.sample_rate = static_cast<int>(rate);
    if (tag == 0x55) {
      s.audio.codec = AudioCodec::kMP3;
      s.audio.codec_name = "mp3";
    } else if (tag == 1 || tag == 3) {
      const unsigned bits = LE16(b.data() + 14);
      if ((bits != 8 && bits != 16 && bits != 24 && bits != 32 && bits != 64) ||
          (tag == 1 && bits > 32) || (tag == 3 && bits != 32 && bits != 64) ||
          LE16(b.data() + 12) != s.audio.channels * bits / 8)
        return false;
      s.audio.codec = AudioCodec::kPCM;
      s.audio.codec_name = std::string("pcm_") +
                           (tag == 3    ? "f"
                            : bits == 8 ? "u"
                                        : "s") +
                           std::to_string(bits) + (bits == 8 ? "" : "le");
      s.audio.extradata = {static_cast<uint8_t>(bits),
                           static_cast<uint8_t>(tag == 3),
                           static_cast<uint8_t>(tag == 1 && bits == 8)};
    } else {
      error_ = "avi: unsupported audio codec";
      return false;
    }
  }
  return true;
}
bool AviDemuxer::Packet(int track, int64_t offset, uint32_t size) {
  auto& t = tracks_[track];
  if (t.index < 0) return true;
  const auto& s = info_.streams[t.index];
  // strh names a stream, but only strf supplies its validated codec and
  // sample layout. A damaged file can omit strf or put movi before it.
  if ((s.type == MediaType::kAudio && s.audio.codec == AudioCodec::kUnknown) ||
      (s.type == MediaType::kVideo && s.video.codec == VideoCodec::kUnknown)) {
    error_ = "avi: packet before stream format";
    return false;
  }
  // Empty video chunks occupy a frame slot (AVI dropped frames).
  if (!size) {
    if (info_.streams[t.index].type == MediaType::kVideo) ++t.units;
    return true;
  }
  if (!t.rate || !t.scale) return false;
  if (s.type == MediaType::kAudio) {
    uint32_t consumed = 0;
    while (consumed < size) {
      uint32_t count = size - consumed;
      int64_t frames = 0;
      if (s.audio.codec == AudioCodec::kMP3) {
        uint8_t header[4];
        AdtsHeader mp3;
        if (!Exact(offset + consumed, header, 4) ||
            !ParseMp3Header(header, 4, &mp3) || mp3.frame_length > count)
          return false;
        count = static_cast<uint32_t>(mp3.frame_length);
        frames = mp3.samples;
      } else {
        const uint32_t block = s.audio.channels * (s.audio.extradata[0] / 8);
        if (count % block) return false;
        count = std::min(count, block * 1024);
        frames = count / block;
      }
      const int64_t pts = t.units * 1000000 / s.audio.sample_rate;
      t.units += frames;
      if (!Add({t.index, offset + consumed, count, pts, pts,
                t.units * 1000000 / s.audio.sample_rate - pts, true}))
        return false;
      consumed += count;
    }
    return true;
  }
  if (t.units > INT64_MAX / (1000000LL * t.scale) - 1) return false;
  const int64_t pts = t.units * 1000000 * t.scale / t.rate;
  ++t.units;
  bool key = false;
  uint8_t head[4096];
  const size_t n = std::min<size_t>(size, sizeof(head));
  if (!Exact(offset, head, n)) return false;
  if (s.video.codec == VideoCodec::kH264) {
    AccessUnitInfo au;
    if (ParseAccessUnit(head, n, false, &au)) key = au.keyframe;
  } else {
    for (size_t i = 0; i + 6 < n; ++i) {
      if (head[i] || head[i + 1] || head[i + 2] != 1) continue;
      if (s.video.codec == VideoCodec::kMPEG4 && head[i + 3] == 0xb6) {
        key = (head[i + 4] >> 6) == 0;
        break;
      }
      if (s.video.codec == VideoCodec::kMPEG2 && head[i + 3] == 0) {
        key = ((head[i + 5] >> 3) & 7) == 1;
        break;
      }
    }
  }
  return Add({t.index, offset, size, pts, pts,
              t.units * 1000000 * t.scale / t.rate - pts, key});
}
bool AviDemuxer::Index(int64_t offset, uint32_t size) {
  uint8_t h[24];
  if (size < sizeof(h) || !Exact(offset, h, sizeof(h))) return false;
  const uint32_t entries = LE32(h + 4);
  const unsigned longs = LE16(h);
  if (h[2] != 0 || (h[3] != 0 && h[3] != 1)) return true;
  if (entries > 1000000 || longs < 2 || longs > 4 ||
      uint64_t(entries) * longs * 4 > size - 24)
    return false;
  if (h[3] == 0) {
    if (longs != 4) return false;
    for (uint32_t i = 0; i < entries; ++i) {
      uint8_t entry[16];
      if (!Exact(offset + 24 + int64_t(i) * 16, entry, 16)) return false;
      uint64_t at = LE64(entry);
      uint32_t bytes = LE32(entry + 8);
      if (at > INT64_MAX - 8 || bytes < 8 || bytes > 16 * 1024 * 1024)
        return false;
      // A super index points only to standard indexes, never another super
      // index.
      uint8_t ih[12];
      if (!Exact(static_cast<int64_t>(at), ih, 12) || ih[11] != 1 ||
          LE32(ih + 4) + 8 > bytes)
        return false;
      if (!Index(static_cast<int64_t>(at) + 8, LE32(ih + 4))) return false;
    }
  } else {
    if (longs != 2) return false;
    const uint64_t base = LE64(h + 12);
    for (uint32_t i = 0; i < entries; ++i) {
      uint8_t entry[8];
      if (!Exact(offset + 24 + int64_t(i) * 8, entry, 8)) return false;
      const uint32_t delta = LE32(entry), length = LE32(entry + 4);
      if (base > static_cast<uint64_t>(INT64_MAX) - delta ||
          keyframes_.size() >= 1000000)
        return false;
      keyframes_[static_cast<int64_t>(base + delta)] = !(length & 0x80000000);
    }
  }
  return true;
}
bool AviDemuxer::Chunks(int64_t begin,
                        int64_t end,
                        int depth,
                        int track,
                        bool movie) {
  if (depth > 8) return false;
  for (int64_t at = begin; at + 8 <= end;) {
    uint8_t h[12];
    if (!Exact(at, h, 8)) return false;
    const uint32_t size = LE32(h + 4);
    const int64_t body = at + 8;
    if (size > uint64_t(end - body)) return false;
    if (!memcmp(h, "LIST", 4)) {
      if (size < 4 || !Exact(body, h + 8, 4)) return false;
      int child = track;
      bool is_movie = movie;
      if (!memcmp(h + 8, "strl", 4)) {
        if (tracks_.size() >= 100) return false;
        child = static_cast<int>(tracks_.size());
        tracks_.push_back({});
      }
      if (!memcmp(h + 8, "movi", 4)) {
        is_movie = true;
        if (movi_ < 0) movi_ = body;
      }
      if (!Chunks(body + 4, body + size, depth + 1, child, is_movie))
        return false;
    } else if (!memcmp(h, "strh", 4) && track >= 0) {
      uint8_t sh[48];
      if (size < 48 || !Exact(body, sh, 48)) return false;
      auto& t = tracks_[track];
      t.scale = LE32(sh + 20);
      t.rate = LE32(sh + 24);
      t.sample_size = LE32(sh + 44);
      if (!memcmp(sh, "vids", 4) || !memcmp(sh, "auds", 4)) {
        StreamInfo s;
        s.type = !memcmp(sh, "vids", 4) ? MediaType::kVideo : MediaType::kAudio;
        t.index = static_cast<int>(info_.streams.size());
        info_.streams.push_back(std::move(s));
      }
    } else if (!memcmp(h, "strf", 4) && track >= 0) {
      if (!Format(track, body, size)) return false;
    } else if (!memcmp(h, "indx", 4) || (h[0] == 'i' && h[1] == 'x')) {
      if (!Index(body, size)) return false;
    } else if (!memcmp(h, "idx1", 4)) {
      if (size % 16 || size / 16 > 1000000) return false;
      for (uint32_t i = 0; i < size; i += 16) {
        uint8_t e[16];
        if (!Exact(body + i, e, 16)) return false;
        const uint32_t off = LE32(e + 8);
        bool key = (LE32(e + 4) & 0x10) != 0;
        if (keyframes_.size() >= 1000000) return false;
        // Different writers use absolute offsets or offsets from movi's type.
        uint8_t chunk[4];
        int64_t pos = movi_ + off;
        if (movi_ < 0 || !Exact(pos, chunk, 4) || (memcmp(chunk, e, 4) != 0))
          pos = off;
        if (!Exact(pos, chunk, 4) || (memcmp(chunk, e, 4) != 0)) return false;
        keyframes_[pos + 8] = key;
      }
    } else if (movie && h[0] >= '0' && h[0] <= '9' && h[1] >= '0' &&
               h[1] <= '9' &&
               ((h[2] == 'd' && (h[3] == 'c' || h[3] == 'b')) ||
                (h[2] == 'w' && h[3] == 'b'))) {
      const int n = (h[0] - '0') * 10 + h[1] - '0';
      if (n >= static_cast<int>(tracks_.size()) || !Packet(n, body, size))
        return false;
    }
    at = body + size + (size & 1);
  }
  return true;
}
bool AviDemuxer::Parse() {
  uint8_t h[12];
  if (!Exact(0, h, 12) || !Detect(h, 12)) return false;
  int64_t at = 0;
  const int64_t file = source_->size();
  do {
    if (!Exact(at, h, 12) || (memcmp(h, "RIFF", 4) != 0) ||
        ((memcmp(h + 8, "AVI ", 4) != 0) && (memcmp(h + 8, "AVIX", 4) != 0)))
      return false;
    const uint32_t size = LE32(h + 4);
    if (size < 4 || at > INT64_MAX - int64_t(size) - 8) return false;
    const int64_t end = at + 8 + size;
    if (file >= 0 && end > file) return false;
    if (!Chunks(at + 12, end, 0, -1, false)) return false;
    at = end + (size & 1);
  } while (file >= 0 && at < file);
  for (auto& s : samples_) {
    auto i = keyframes_.find(s.offset);
    if (i != keyframes_.end() &&
        info_.streams[s.stream].type == MediaType::kVideo)
      s.key = i->second;
  }
  for (const auto& s : info_.streams)
    if (s.type == MediaType::kVideo && s.video.codec == VideoCodec::kUnknown)
      return false;
  return true;
}
}  // namespace lychee
