#include "media/codec.h"

namespace lychee {

const char* MediaTypeName(MediaType type) {
  switch (type) {
    case MediaType::kVideo:
      return "video";
    case MediaType::kAudio:
      return "audio";
    case MediaType::kSubtitle:
      return "subtitle";
    case MediaType::kUnknown:
      break;
  }
  return "unknown";
}

const char* VideoCodecName(VideoCodec codec) {
  switch (codec) {
    case VideoCodec::kH264:
      return "h264";
    case VideoCodec::kHEVC:
      return "hevc";
    case VideoCodec::kVP8:
      return "vp8";
    case VideoCodec::kVP9:
      return "vp9";
    case VideoCodec::kAV1:
      return "av1";
    case VideoCodec::kMPEG2:
      return "mpeg2";
    case VideoCodec::kMPEG4:
      return "mpeg4";
    case VideoCodec::kOther:
      return "other";
    case VideoCodec::kUnknown:
      break;
  }
  return "unknown";
}

const char* AudioCodecName(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kAAC:
      return "aac";
    case AudioCodec::kMP3:
      return "mp3";
    case AudioCodec::kOpus:
      return "opus";
    case AudioCodec::kVorbis:
      return "vorbis";
    case AudioCodec::kFLAC:
      return "flac";
    case AudioCodec::kALAC:
      return "alac";
    case AudioCodec::kAC3:
      return "ac3";
    case AudioCodec::kEAC3:
      return "eac3";
    case AudioCodec::kPCM:
      return "pcm";
    case AudioCodec::kOther:
      return "other";
    case AudioCodec::kUnknown:
      break;
  }
  return "unknown";
}

}  // namespace lychee
