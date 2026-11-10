#ifndef LYCHEE_MEDIA_STREAM_INFO_H_
#define LYCHEE_MEDIA_STREAM_INFO_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
  // Frames per second as the container says (MP4's most common sample
  // duration, Matroska's DefaultDuration, FFmpeg's average), 0 if it
  // does not.
  double frame_rate = 0;
  ColorInfo color;
  // Codec configuration in the container's format (e.g. avcC, hvcC).
  std::vector<uint8_t> extradata;
  std::shared_ptr<const CodecPrivate> codec_private;
  // Each frame is due soon after its packet (live input): decode without a
  // pipeline that holds frames back.
  bool low_delay = false;

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
  // The shared demuxer has decoded timing/text/pictures on the read thread.
  bool normalized = false;
};

// What a track is for, as the container says (lychee_track_role).
enum class TrackRole {
  kMain = 0,
  // Spoken commentary (a director's, ...).
  kCommentary = 1,
  // For viewers who cannot see the picture: audio description, or text
  // describing the video.
  kDescription = 2,
  // For viewers who cannot hear the sound: captions (SDH), or audio with
  // clearer dialog.
  kHearingImpaired = 3,
};

// The role of Apple's media characteristics, a comma separated list of
// UTIs (HLS CHARACTERISTICS, MP4 'tagc').
inline TrackRole TrackRoleFromCharacteristics(std::string_view list) {
  if (list.find("public.accessibility.describes-video") !=
      std::string_view::npos) {
    return TrackRole::kDescription;
  }
  if (list.find("public.accessibility.transcribes-spoken-dialog") !=
          std::string_view::npos ||
      list.find("public.accessibility.describes-music-and-sound") !=
          std::string_view::npos) {
    return TrackRole::kHearingImpaired;
  }
  return TrackRole::kMain;
}

struct StreamInfo {
  int index = -1;
  MediaType type = MediaType::kUnknown;
  int64_t duration = kNoTimestamp;
  std::string language;
  std::string title;
  // The container marks it as the track to play by default.
  bool default_track = false;
  // ... or as one to show even when subtitles are off (foreign dialog).
  bool forced = false;
  TrackRole role = TrackRole::kMain;
  // Bits per second the container declares or its sizes imply; 0 if
  // unknown.
  int64_t bitrate = 0;
  VideoConfig video;
  AudioConfig audio;
  SubtitleConfig subtitle;
};

struct MediaInfo {
  std::string format_name;
  // kNoTimestamp if unknown (e.g. live streams).
  int64_t duration = kNoTimestamp;
  int64_t start_time = 0;
  // The position (media time less |start_time|) the first packets play
  // at: 0 but for live HLS, which starts near the end of its window.
  int64_t start_position = 0;
  bool seekable = false;
  // The input arrives as it is made and cannot be held back (an HTTP-FLV
  // live stream): the player plays a little faster while it has much of
  // it buffered, so it does not fall further behind (Player's catch-up).
  bool realtime = false;
  std::vector<StreamInfo> streams;
  // Indices into |streams| the demuxer suggests, or -1.
  int default_video_stream = -1;
  int default_audio_stream = -1;
  // Only a stream the container marks as default (or forced).
  int default_subtitle_stream = -1;
  // Container tags (title, artist, album, ...), keys lowercase.
  std::vector<std::pair<std::string, std::string>> metadata;
  // The cover picture as the container stores it (JPEG, PNG, ...), or null.
  std::shared_ptr<const std::vector<uint8_t>> artwork;
};

inline int VideoConfig::display_width() const {
  if (sar_num <= 0 || sar_den <= 0 || sar_num == sar_den) {
    return width;
  }
  return static_cast<int>(static_cast<int64_t>(width) * sar_num / sar_den);
}

}  // namespace lychee

#endif  // LYCHEE_MEDIA_STREAM_INFO_H_
