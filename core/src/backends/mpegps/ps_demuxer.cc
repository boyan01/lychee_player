#include "backends/mpegps/ps_demuxer.h"
#include "backends/mp2t/elementary_stream.h"

namespace lychee {
namespace {
int64_t Timestamp(const uint8_t* p) {
  if (!(p[0] & 1) || !(p[2] & 1) || !(p[4] & 1)) return -1;
  const int64_t ticks = (int64_t(p[0] & 14) << 29) | (int64_t(p[1]) << 22) |
                        (int64_t(p[2] & 254) << 14) | (int64_t(p[3]) << 7) |
                        (p[4] >> 1);
  return ticks * 1000000 / 90000;
}
}  // namespace
bool PsDemuxer::Video(int64_t physical,
                      uint32_t size,
                      int64_t pts,
                      int64_t dts) {
  if (extents_.size() >= 1000000 ||
      pending_.size() + size > size_t{16} * 1024 * 1024)
    return false;
  extents_.push_back({logical_, physical, size});
  if (pts >= 0) stamps_.push_back({logical_, pts, dts});
  logical_ += size;
  const size_t at = pending_.size();
  pending_.resize(at + size);
  return Exact(physical, pending_.data() + at, size) && Pictures(false);
}
bool PsDemuxer::Pictures(bool final) {
  for (;;) {
    size_t picture = SIZE_MAX, prefix = SIZE_MAX, cut = SIZE_MAX;
    for (size_t i = 0; i + 4 <= pending_.size(); ++i) {
      if (pending_[i] || pending_[i + 1] || pending_[i + 2] != 1) continue;
      const uint8_t code = pending_[i + 3];
      if (picture != SIZE_MAX && (code == 0xb3 || code == 0xb8) &&
          prefix == SIZE_MAX)
        prefix = i;
      if (code != 0) continue;
      if (picture != SIZE_MAX) {
        cut = prefix == SIZE_MAX ? i : prefix;
        break;
      }
      picture = i;
    }
    if (cut != SIZE_MAX) {
      if (!Picture(cut)) return false;
      pending_.erase(pending_.begin(),
                     pending_.begin() + static_cast<ptrdiff_t>(cut));
      pending_offset_ += static_cast<int64_t>(cut);
      continue;
    }
    if (final && !pending_.empty()) {
      if (picture == SIZE_MAX || !Picture(pending_.size())) return false;
      pending_.clear();
    }
    return true;
  }
}
bool PsDemuxer::Picture(size_t size) {
  size_t picture = SIZE_MAX;
  for (size_t i = 0; i + 8 <= size; ++i) {
    if (pending_[i] || pending_[i + 1] || pending_[i + 2] != 1) continue;
    if (pending_[i + 3] == 0xb3) {
      const int width = (pending_[i + 4] << 4) | (pending_[i + 5] >> 4);
      const int height = ((pending_[i + 5] & 15) << 8) | pending_[i + 6];
      static constexpr int numerator[] = {0,  24000, 24,    25, 30000,
                                          30, 50,    60000, 60};
      static constexpr int denominator[] = {1, 1001, 1, 1, 1001, 1, 1, 1001, 1};
      const unsigned code = pending_[i + 7] & 15;
      if (!width || !height || code < 1 || code > 8) return false;
      frame_us_ = 1000000LL * denominator[code] / numerator[code];
      if (video_ < 0) {
        StreamInfo s;
        s.type = MediaType::kVideo;
        s.video.codec = VideoCodec::kMPEG2;
        s.video.codec_name = "mpeg2video";
        s.video.width = width;
        s.video.height = height;
        const unsigned aspect = pending_[i + 7] >> 4;
        if (aspect == 2) {
          s.video.sar_num = 4 * height;
          s.video.sar_den = 3 * width;
        }
        if (aspect == 3) {
          s.video.sar_num = 16 * height;
          s.video.sar_den = 9 * width;
        }
        video_ = static_cast<int>(info_.streams.size());
        info_.streams.push_back(std::move(s));
      }
    }
    if (pending_[i + 3] == 0) {
      picture = i;
      break;
    }
  }
  if (video_ < 0 || picture == SIZE_MAX || picture + 6 > size) return false;
  if (sequence_.empty() && picture > 0 && picture < 65536) {
    sequence_.assign(pending_.begin(),
                     pending_.begin() + static_cast<ptrdiff_t>(picture));
    info_.streams[video_].video.extradata = sequence_;
  }
  const int temporal =
      (pending_[picture + 4] << 2) | (pending_[picture + 5] >> 6);
  const int kind = (pending_[picture + 5] >> 3) & 7;
  if (kind < 1 || kind > 3) return false;
  bool fresh = false;
  while (stamp_ < stamps_.size() &&
         stamps_[stamp_].offset <=
             pending_offset_ + static_cast<int64_t>(picture)) {
    const auto& t = stamps_[stamp_++];
    gop_pts_ = t.pts - int64_t(temporal) * frame_us_;
    if (t.dts >= 0) next_dts_ = t.dts;
    fresh = true;
  }
  // A GOP without an explicit PTS starts where the preceding picture ends.
  if (kind == 1 && !fresh) gop_pts_ = next_dts_ - int64_t(temporal) * frame_us_;
  const int64_t pts = gop_pts_ + int64_t(temporal) * frame_us_;
  const int64_t dts = next_dts_;
  next_dts_ += frame_us_;
  return Add({video_, pending_offset_, static_cast<uint32_t>(size), pts, dts,
              frame_us_, kind == 1});
}
bool PsDemuxer::Audio(int64_t physical, uint32_t size, int64_t pts, bool ac3) {
  if (audio_extents_.size() >= 1000000 ||
      audio_pending_.size() + size > size_t{1024} * 1024)
    return false;
  audio_extents_.push_back({audio_logical_, physical, size});
  audio_logical_ += size;
  if (audio_pending_.empty() && pts >= 0) audio_pts_ = pts;
  const size_t old = audio_pending_.size();
  audio_pending_.resize(old + size);
  if (!Exact(physical, audio_pending_.data() + old, size)) return false;
  size_t consumed = 0;
  while (audio_pending_.size() - consumed >= 8) {
    const uint8_t* h = audio_pending_.data() + consumed;
    const size_t available = audio_pending_.size() - consumed;
    AdtsHeader format;
    AudioCodec codec = AudioCodec::kUnknown;
    const char* name = "";
    if (ac3) {
      if (!ParseAc3Header(h, available, &format)) {
        ++consumed;
        continue;
      }
      static constexpr unsigned bitrates[] = {32,  40,  48,  56,  64,  80,  96,
                                              112, 128, 160, 192, 224, 256, 320,
                                              384, 448, 512, 576, 640};
      static constexpr unsigned words44[] = {
          69,  87,  104, 121, 139, 174, 208,  243,  278, 348,
          417, 487, 557, 696, 835, 975, 1114, 1253, 1393};
      const unsigned index = h[4] & 63, rate = h[4] >> 6;
      if (index > 37 || rate > 2) return false;
      format.frame_length = rate == 0 ? bitrates[index / 2] * 4
                            : rate == 2
                                ? bitrates[index / 2] * 6
                                : (words44[index / 2] + (index & 1)) * 2;
      format.samples = 1536;
      codec = AudioCodec::kAC3;
      name = "ac3";
    } else if (ParseMp3Header(h, available, &format)) {
      codec = AudioCodec::kMP3;
      name = "mp3";
    } else if (h[0] == 255 && (h[1] & 0xe0) == 0xe0 && ((h[1] >> 1) & 3) == 2) {
      const unsigned version = (h[1] >> 3) & 3, br = h[2] >> 4,
                     sr = (h[2] >> 2) & 3;
      static constexpr int rates[] = {44100, 48000, 32000};
      static constexpr int high[] = {0,   32,  48,  56,  64,  80,  96, 112,
                                     128, 160, 192, 224, 256, 320, 384};
      static constexpr int low[] = {0,  8,  16, 24,  32,  40,  48, 56,
                                    64, 80, 96, 112, 128, 144, 160};
      if (version == 1 || sr == 3 || br == 0 || br == 15) return false;
      format.sample_rate = rates[sr] >> (version == 3   ? 0
                                         : version == 2 ? 1
                                                        : 2);
      format.channels = (h[3] >> 6) == 3 ? 1 : 2;
      format.samples = 1152;
      format.frame_length =
          144000 * (version == 3 ? high[br] : low[br]) / format.sample_rate +
          ((h[2] >> 1) & 1);
      codec = AudioCodec::kMP2;
      name = "mp2";
    } else {
      ++consumed;
      continue;
    }
    if (format.frame_length > available) break;
    if (audio_ < 0) {
      StreamInfo stream;
      stream.type = MediaType::kAudio;
      stream.audio.codec = codec;
      stream.audio.codec_name = name;
      stream.audio.sample_rate = format.sample_rate;
      stream.audio.channels = format.channels;
      audio_ = static_cast<int>(info_.streams.size());
      info_.streams.push_back(std::move(stream));
    }
    if (info_.streams[audio_].audio.codec != codec) return false;
    if (audio_pts_ < 0) audio_pts_ = 0;
    const int64_t duration =
        int64_t(format.samples) * 1000000 / format.sample_rate;
    if (!Add({audio_, audio_offset_ + static_cast<int64_t>(consumed),
              static_cast<uint32_t>(format.frame_length), audio_pts_,
              audio_pts_, duration, true}))
      return false;
    audio_pts_ += duration;
    consumed += format.frame_length;
  }
  audio_pending_.erase(
      audio_pending_.begin(),
      audio_pending_.begin() + static_cast<ptrdiff_t>(consumed));
  audio_offset_ += static_cast<int64_t>(consumed);
  return true;
}
bool PsDemuxer::Parse() {
  uint8_t h[32];
  if (!Exact(0, h, 4) || !Detect(h, 4)) return false;
  const int64_t file = source_->size();
  if (file < 0) {
    error_ = "mpegps: a finite seekable source is required";
    return false;
  }
  int64_t at = 0;
  while (at + 4 <= file) {
    if (!Exact(at, h, 4)) return false;
    if (h[0] || h[1] || h[2] != 1) {
      ++at;
      continue;
    }
    const uint8_t id = h[3];
    if (id == 0xb9) break;
    if (id == 0xba) {
      if (!Exact(at, h, 14)) return false;
      const int64_t size = (h[4] & 0xc0) == 0x40   ? 14 + (h[13] & 7)
                           : (h[4] & 0xf0) == 0x20 ? 12
                                                   : 0;
      if (!size || size > file - at) return false;
      at += size;
      continue;
    }
    if (id < 0xbb || !Exact(at, h, 6)) return false;
    uint32_t size = (uint32_t(h[4]) << 8) | h[5];
    int64_t end = at + 6 + size;
    if (size == 0 && id >= 0xe0 && id <= 0xef) {
      end = at + 6;
      while (end + 4 <= file) {
        if (end - at > int64_t{16} * 1024 * 1024 || !Exact(end, h, 4))
          return false;
        if (h[0] == 0 && h[1] == 0 && h[2] == 1 && h[3] >= 0xb9) break;
        ++end;
      }
      if (end + 4 > file) end = file;
    }
    if (end > file || end <= at + 6) return false;
    if (id == 0xe0 || (id >= 0xc0 && id <= 0xdf) || id == 0xbd) {
      int64_t body = at + 6, pts = -1, dts = -1;
      if (!Exact(body, h, 3)) return false;
      if ((h[0] & 0xc0) == 0x80) {
        const unsigned flags = h[1] >> 6;
        const unsigned extra = h[2];
        if ((h[0] & 0x30) || flags == 1 || body + 3 + extra > end) return false;
        if (flags >= 2) {
          if (extra < 5 || !Exact(body + 3, h + 3, flags == 3 ? 10 : 5))
            return false;
          pts = Timestamp(h + 3);
          if (flags == 3) {
            if (extra < 10) return false;
            dts = Timestamp(h + 8);
          }
        }
        body += 3 + extra;
      } else {
        if (!Exact(body, h, 1)) return false;
        while (h[0] == 0xff && body < end) {
          ++body;
          if (!Exact(body, h, 1)) return false;
        }
        if ((h[0] & 0xc0) == 0x40) {
          body += 2;
          if (!Exact(body, h, 1)) return false;
        }
        if ((h[0] & 0xf0) == 0x20 || (h[0] & 0xf0) == 0x30) {
          const bool both = (h[0] & 0xf0) == 0x30;
          if (!Exact(body, h, both ? 10 : 5)) return false;
          pts = Timestamp(h);
          if (both) dts = Timestamp(h + 5);
          body += both ? 10 : 5;
        } else if (h[0] == 15)
          ++body;
        else
          return false;
      }
      if (body > end) return false;
      if (id == 0xe0) {
        if (!Video(body, static_cast<uint32_t>(end - body), pts, dts))
          return false;
      } else if (id == 0xbd) {
        if (end - body >= 4 && Exact(body, h, 4) && h[0] >= 0x80 &&
            h[0] <= 0x87) {
          if (audio_id_ < 0) {
            audio_id_ = id;
            substream_id_ = h[0];
          }
          if (audio_id_ == id && substream_id_ == h[0] &&
              !Audio(body + 4, static_cast<uint32_t>(end - body - 4), pts,
                     true))
            return false;
        }
      } else {
        if (audio_id_ < 0) audio_id_ = id;
        if (audio_id_ == id &&
            !Audio(body, static_cast<uint32_t>(end - body), pts, false))
          return false;
      }
    }
    at = end;
  }
  if (!Pictures(true)) return false;
  int64_t origin = INT64_MAX;
  for (const auto& s : samples_)
    origin = std::min(origin, s.pts);
  if (origin != INT64_MAX) info_.start_time = origin;
  return true;
}
bool PsDemuxer::Load(const Sample& sample, std::vector<uint8_t>* bytes) {
  const auto& extents = sample.stream == video_ ? extents_ : audio_extents_;
  const size_t prefix =
      sample.stream == video_ && sample.key ? sequence_.size() : 0;
  bytes->resize(prefix + sample.size);
  if (prefix) memcpy(bytes->data(), sequence_.data(), prefix);
  int64_t logical = sample.offset;
  size_t done = 0;
  auto it = std::upper_bound(
      extents.begin(), extents.end(), logical,
      [](int64_t at, const Extent& e) { return at < e.logical; });
  if (it == extents.begin()) return false;
  --it;
  while (done < sample.size && it != extents.end()) {
    const int64_t within = logical - it->logical;
    if (within < 0 || within >= it->size) return false;
    const size_t count = std::min<size_t>(
        sample.size - done, it->size - static_cast<size_t>(within));
    if (!Exact(it->physical + within, bytes->data() + prefix + done, count))
      return false;
    done += count;
    logical += static_cast<int64_t>(count);
    ++it;
  }
  return done == sample.size;
}
}  // namespace lychee
