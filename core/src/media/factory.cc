#include "media/factory.h"

#include "media/cpu_frame_converter.h"

#if defined(LYCHEE_WITH_FFMPEG)
#include "backends/ffmpeg/ffmpeg_audio_decoder.h"
#include "backends/ffmpeg/ffmpeg_demuxer.h"
#include "backends/ffmpeg/ffmpeg_subtitle_decoder.h"
#include "backends/ffmpeg/ffmpeg_video_decoder.h"
#endif

#if defined(__APPLE__)
#include "backends/apple/apple_frame_converter.h"
#include "backends/apple/core_audio_sink.h"
#elif defined(__ANDROID__)
#include "backends/android/aaudio_sink.h"
#elif defined(_WIN32)
#include "backends/windows/wasapi_sink.h"
#elif defined(__linux__)
#include "backends/linux/pulse_audio_sink.h"
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

std::unique_ptr<SubtitleDecoder> CreateSubtitleDecoder() {
#if defined(LYCHEE_WITH_FFMPEG)
  return std::make_unique<FFmpegSubtitleDecoder>();
#else
  return nullptr;
#endif
}

std::unique_ptr<AudioSink> CreateAudioSink() {
#if defined(__APPLE__)
  return std::make_unique<CoreAudioSink>();
#elif defined(__ANDROID__)
  return std::make_unique<AAudioSink>();
#elif defined(_WIN32)
  return std::make_unique<WasapiSink>();
#elif defined(__linux__)
  return std::make_unique<PulseAudioSink>();
#else
  return nullptr;
#endif
}

std::unique_ptr<FrameConverter> CreateFrameConverter() {
#if defined(__APPLE__)
  return std::make_unique<AppleFrameConverter>();
#else
  return std::make_unique<CpuFrameConverter>();
#endif
}

}  // namespace lychee
