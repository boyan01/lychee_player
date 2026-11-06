#ifndef LYCHEE_MEDIA_CODEC_H_
#define LYCHEE_MEDIA_CODEC_H_

namespace lychee {

enum class MediaType { kUnknown, kVideo, kAudio, kSubtitle };

enum class VideoCodec {
  kUnknown,
  kH264,
  kHEVC,
  kVP8,
  kVP9,
  kAV1,
  kMPEG2,
  kMPEG4,
  kWMV1,
  kWMV2,
  kWMV3,
  kVC1,
  kOther,
};

enum class AudioCodec {
  kUnknown,
  kAAC,
  kMP3,
  kMP2,
  kOpus,
  kVorbis,
  kFLAC,
  kALAC,
  kAC3,
  kEAC3,
  kPCM,
  kWMA2,
  kWMAPro,
  kWMALossless,
  kOther,
};

const char* MediaTypeName(MediaType type);
const char* VideoCodecName(VideoCodec codec);
const char* AudioCodecName(AudioCodec codec);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_CODEC_H_
