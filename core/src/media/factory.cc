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
#include <TargetConditionals.h>

#include "backends/apple/apple_frame_converter.h"
#include "backends/apple/core_audio_sink.h"
#if !defined(LYCHEE_WITH_FFMPEG)
#include <mutex>

#include "backends/apple/at_audio_decoder.h"
#include "backends/apple/vt_video_decoder.h"
#include "backends/matroska/matroska_demuxer.h"
#include "backends/mp4/mp4_demuxer.h"
#include "media/text_subtitle_decoder.h"
#endif
#elif defined(__ANDROID__)
#include <android/native_window.h>

#include "backends/android/aaudio_sink.h"
#include "backends/android/media_codec_video_decoder.h"
#if !defined(LYCHEE_WITH_FFMPEG)
#include "backends/android/media_codec_audio_decoder.h"
#include "backends/mp4/mp4_demuxer.h"
#endif
#elif defined(_WIN32)
#include <unknwn.h>

#include "backends/windows/wasapi_sink.h"
#elif defined(__linux__)
#include "backends/linux/pulse_audio_sink.h"
#endif

namespace lychee {

#if defined(__APPLE__) && !defined(LYCHEE_WITH_FFMPEG)
namespace {

// MP4, else Matroska. Mp4Demuxer opens the input as before; if it fails,
// MatroskaDemuxer opens it a second time and gives up at the first bytes
// unless they are the EBML magic, so other inputs keep MP4's error.
class ProbingDemuxer : public Demuxer {
 public:
  const char* name() const override {
    return demuxer_ ? demuxer_->name() : "probe";
  }

  bool Open(const std::string& url, std::string* error) override {
    if (!Use(std::make_unique<Mp4Demuxer>())) {
      *error = "aborted";
      return false;
    }
    if (demuxer_->Open(url, error)) {
      return true;
    }
    const std::string mp4_error = *error;
    if (!Use(std::make_unique<MatroskaDemuxer>())) {
      *error = "aborted";
      return false;
    }
    if (demuxer_->Open(url, error)) {
      return true;
    }
    if (*error == MatroskaDemuxer::kNotMatroska) {
      *error = mp4_error;
    }
    return false;
  }

  const MediaInfo& media_info() const override {
    static const MediaInfo kNone;
    return demuxer_ ? demuxer_->media_info() : kNone;
  }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override {
    demuxer_->SetEnabledStreams(stream_indices);
  }
  DemuxerReadResult Read(EncodedPacket* packet) override {
    return demuxer_->Read(packet);
  }
  bool Seek(int64_t time) override { return demuxer_->Seek(time); }
  void Interrupt() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (demuxer_) {
      demuxer_->Interrupt();
    }
  }
  void Abort() override {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    if (demuxer_) {
      demuxer_->Abort();
    }
  }
  std::string error() const override {
    return demuxer_ ? demuxer_->error() : std::string();
  }

 private:
  // False if aborted meanwhile.
  bool Use(std::unique_ptr<Demuxer> demuxer) {
    std::lock_guard<std::mutex> lock(mutex_);
    demuxer_ = std::move(demuxer);
    return !aborted_;
  }

  // Interrupt() and Abort() come from other threads while Open() runs.
  std::mutex mutex_;
  std::unique_ptr<Demuxer> demuxer_;
  bool aborted_ = false;
};

}  // namespace
#endif

std::unique_ptr<Demuxer> CreateDemuxer() {
#if defined(LYCHEE_WITH_FFMPEG)
  return std::make_unique<FFmpegDemuxer>();
#elif defined(__APPLE__)
  return std::make_unique<ProbingDemuxer>();
#elif defined(__ANDROID__)
  return std::make_unique<Mp4Demuxer>();
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
#if defined(__ANDROID__) && defined(LYCHEE_WITH_FFMPEG)
  if (allow_hardware && accepts(VideoFrameStorage::kAndroidSurface)) {
    decoders.push_back(std::make_unique<MediaCodecVideoDecoder>(
        surface, rotate_surface_frames));
  }
#elif defined(__ANDROID__)
  // Without FFmpeg the platform's software decoders are the fallback, also
  // when a hardware codec fails (the emulator's takes no HEVC Main 10).
  if (accepts(VideoFrameStorage::kAndroidSurface)) {
    if (allow_hardware) {
      decoders.push_back(std::make_unique<MediaCodecVideoDecoder>(
          surface, rotate_surface_frames,
          MediaCodecVideoDecoder::Codecs::kHardwareOnly));
    }
    decoders.push_back(std::make_unique<MediaCodecVideoDecoder>(
        surface, rotate_surface_frames,
        MediaCodecVideoDecoder::Codecs::kSoftwareOnly));
  }
#endif
#if defined(__APPLE__) && !defined(LYCHEE_WITH_FFMPEG)
#if TARGET_OS_OSX
  if (allow_hardware) {
    decoders.push_back(std::make_unique<VTVideoDecoder>(true));
  }
  decoders.push_back(std::make_unique<VTVideoDecoder>(false));
#else
  // iOS lets apps pick neither: VideoToolbox decides.
  decoders.push_back(std::make_unique<VTVideoDecoder>(true));
#endif
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
#if defined(__APPLE__) && !defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<ATAudioDecoder>());
#elif defined(__ANDROID__) && !defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<MediaCodecAudioDecoder>());
#endif
#if defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<FFmpegAudioDecoder>());
#endif
  return decoders;
}

std::unique_ptr<SubtitleDecoder> CreateSubtitleDecoder() {
#if defined(LYCHEE_WITH_FFMPEG)
  return std::make_unique<FFmpegSubtitleDecoder>();
#elif defined(__APPLE__)
  return std::make_unique<TextSubtitleDecoder>();
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
