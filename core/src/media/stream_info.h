#ifndef LYCHEE_MEDIA_STREAM_INFO_H_
#define LYCHEE_MEDIA_STREAM_INFO_H_

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/time.h"
#include "media/codec.h"

namespace lychee {

// Backend specific codec parameters a demuxer can attach to a config, e.g.
// the FFmpeg demuxer attaches AVCodecParameters so FFmpeg decoders can decode
// every codec FFmpeg knows about. Decoders from other backends ignore it and
// use the portable fields.
struct CodecPrivate {
  virtual ~CodecPrivate() = default;
};

// ITU-T H.273 code points. 2 means unspecified.
struct ColorInfo {
  uint8_t primaries = 2;
  uint8_t transfer = 2;
  uint8_t matrix = 2;
  bool full_range = false;
};

struct VideoConfig {
  VideoCodec codec = VideoCodec::kUnknown;
  std::string codec_name;
  int width = 0;
  int height = 0;
  int sar_num = 1;
  int sar_den = 1;
  // Clockwise, in degrees.
  int rotation = 0;
  ColorInfo color;
  // Codec configuration in the container's format (e.g. avcC, hvcC).
  std::vector<uint8_t> extradata;
  std::shared_ptr<const CodecPrivate> codec_private;

  // Width with the sample aspect ratio applied.
  int display_width() const;
};

struct AudioConfig {
  AudioCodec codec = AudioCodec::kUnknown;
  std::string codec_name;
  int sample_rate = 0;
  int channels = 0;
  std::vector<uint8_t> extradata;
  std::shared_ptr<const CodecPrivate> codec_private;
};

struct SubtitleConfig {
  std::string codec_name;
  // Timed text (SRT, WebVTT, mov_text, ASS, ...) rather than bitmaps.
  bool text = false;
  std::vector<uint8_t> extradata;
  std::shared_ptr<const CodecPrivate> codec_private;
};

struct StreamInfo {
  int index = -1;
  MediaType type = MediaType::kUnknown;
  int64_t duration = kNoTimestamp;
  std::string language;
  std::string title;
  // The container marks it as the track to play by default.
  bool default_track = false;
  VideoConfig video;
  AudioConfig audio;
  SubtitleConfig subtitle;
};

struct MediaInfo {
  std::string format_name;
  // kNoTimestamp if unknown (e.g. live streams).
  int64_t duration = kNoTimestamp;
  int64_t start_time = 0;
  bool seekable = false;
  std::vector<StreamInfo> streams;
  // Indices into |streams| the demuxer suggests, or -1.
  int default_video_stream = -1;
  int default_audio_stream = -1;
  // Only a stream the container marks as default (or forced).
  int default_subtitle_stream = -1;
  // Container tags (title, artist, album, ...), keys lowercase.
  std::vector<std::pair<std::string, std::string>> metadata;
};

inline int VideoConfig::display_width() const {
  if (sar_num <= 0 || sar_den <= 0 || sar_num == sar_den) {
    return width;
  }
  return static_cast<int>(static_cast<int64_t>(width) * sar_num / sar_den);
}

}  // namespace lychee

#endif  // LYCHEE_MEDIA_STREAM_INFO_H_
