#include "media/factory.h"

#if defined(LYCHEE_WITH_FFMPEG)
#include "backends/ffmpeg/ffmpeg_audio_decoder.h"
#include "backends/ffmpeg/ffmpeg_demuxer.h"
#include "backends/ffmpeg/ffmpeg_video_decoder.h"
#endif

#if defined(__APPLE__)
#include "backends/apple/apple_frame_converter.h"
#include "backends/apple/core_audio_sink.h"
#endif

namespace lychee {

std::unique_ptr<Demuxer> CreateDemuxer() {
#if defined(LYCHEE_WITH_FFMPEG)
  return std::make_unique<FFmpegDemuxer>();
#else
  return nullptr;
#endif
}

std::vector<std::unique_ptr<VideoDecoder>> CreateVideoDecoders(
    bool allow_hardware) {
  std::vector<std::unique_ptr<VideoDecoder>> decoders;
#if defined(LYCHEE_WITH_FFMPEG)
  if (allow_hardware) {
    decoders.push_back(std::make_unique<FFmpegVideoDecoder>(true));
  }
  decoders.push_back(std::make_unique<FFmpegVideoDecoder>(false));
#endif
  return decoders;
}

std::vector<std::unique_ptr<AudioDecoder>> CreateAudioDecoders() {
  std::vector<std::unique_ptr<AudioDecoder>> decoders;
#if defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<FFmpegAudioDecoder>());
#endif
  return decoders;
}

std::unique_ptr<AudioSink> CreateAudioSink() {
#if defined(__APPLE__)
  return std::make_unique<CoreAudioSink>();
#else
  return nullptr;
#endif
}

std::unique_ptr<FrameConverter> CreateFrameConverter() {
#if defined(__APPLE__)
  return std::make_unique<AppleFrameConverter>();
#else
  return nullptr;
#endif
}

}  // namespace lychee
