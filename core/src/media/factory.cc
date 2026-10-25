#include "media/factory.h"

#include "base/time.h"
#include "media/cpu_frame_converter.h"
#include "media/null_audio_sink.h"

#if defined(LYCHEE_WITH_FFMPEG)
#include "backends/ffmpeg/ffmpeg_audio_decoder.h"
#include "backends/ffmpeg/ffmpeg_demuxer.h"
#include "backends/ffmpeg/ffmpeg_subtitle_decoder.h"
#include "backends/ffmpeg/ffmpeg_video_decoder.h"
#endif

#if defined(LYCHEE_WITH_LIBASS)
#include "backends/libass/libass_rasterizer.h"
#endif

#if defined(__APPLE__)
#include "backends/apple/apple_frame_converter.h"
#include "backends/apple/core_audio_sink.h"
#elif defined(__ANDROID__)
#include <android/native_window.h>

#include "backends/android/aaudio_sink.h"
#include "backends/android/media_codec_video_decoder.h"
#elif defined(_WIN32)
#include <unknwn.h>

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
    bool allow_hardware,
    uint32_t accepted_storage,
    [[maybe_unused]] const VideoDevice& device,
    [[maybe_unused]] const std::shared_ptr<void>& surface,
    [[maybe_unused]] bool rotate_surface_frames) {
  std::vector<std::unique_ptr<VideoDecoder>> decoders;
  [[maybe_unused]] auto accepts = [accepted_storage](VideoFrameStorage storage) {
    return (accepted_storage & static_cast<uint32_t>(storage)) != 0;
  };
#if defined(__ANDROID__)
  if (allow_hardware && accepts(VideoFrameStorage::kAndroidSurface)) {
    decoders.push_back(std::make_unique<MediaCodecVideoDecoder>(
        surface, rotate_surface_frames));
  }
#endif
#if defined(LYCHEE_WITH_FFMPEG)
  if (allow_hardware) {
    decoders.push_back(
        std::make_unique<FFmpegVideoDecoder>(true, accepted_storage, device));
  }
  decoders.push_back(
      std::make_unique<FFmpegVideoDecoder>(false, accepted_storage));
#endif
  return decoders;
}

bool MakeVideoDevice(int32_t type,
                     [[maybe_unused]] void* handle,
                     [[maybe_unused]] const char* path,
                     VideoDevice* device) {
  *device = VideoDevice();
  switch (static_cast<VideoDevice::Type>(type)) {
    case VideoDevice::Type::kNone:
      return true;
#if defined(_WIN32)
    case VideoDevice::Type::kD3D11Adapter: {
      if (!handle) {
        return false;
      }
      auto* adapter = static_cast<IUnknown*>(handle);
      adapter->AddRef();
      device->type = VideoDevice::Type::kD3D11Adapter;
      device->handle = std::shared_ptr<void>(adapter, [](void* retained) {
        static_cast<IUnknown*>(retained)->Release();
      });
      return true;
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    case VideoDevice::Type::kDrmRenderNode:
      device->type = VideoDevice::Type::kDrmRenderNode;
      device->path = path ? path : "";
      return true;
#endif
    default:
      return false;
  }
}

std::shared_ptr<void> RetainVideoSurface([[maybe_unused]] void* surface) {
#if defined(__ANDROID__)
  if (!surface) {
    return nullptr;
  }
  auto* window = static_cast<ANativeWindow*>(surface);
  ANativeWindow_acquire(window);
  return std::shared_ptr<void>(window, [](void* retained) {
    ANativeWindow_release(static_cast<ANativeWindow*>(retained));
  });
#else
  return nullptr;
#endif
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

std::unique_ptr<SubtitleRasterizer> CreateSubtitleRasterizer(
    const SubtitleConfig& config) {
#if defined(LYCHEE_WITH_LIBASS)
  if (config.codec_name == "ass" || config.codec_name == "ssa") {
    return std::make_unique<LibassRasterizer>();
  }
#else
  (void)config;
#endif
  return nullptr;
}

std::unique_ptr<AudioSink> CreateAudioSink() {
  if (IsVirtualTime()) {
    return std::make_unique<NullAudioSink>();
  }
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
