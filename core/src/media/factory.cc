#include "media/factory.h"

#include <atomic>

#include "base/time.h"
#include "media/cpu_frame_converter.h"
#if defined(__ANDROID__) && !defined(LYCHEE_WITH_FFMPEG)
#include "media/ffmpeg_extension.h"
#endif
#include "media/null_audio_sink.h"

#if defined(LYCHEE_WITH_FFMPEG)
#include "backends/ffmpeg/ffmpeg_audio_decoder.h"
#include "backends/ffmpeg/ffmpeg_common.h"
#include "backends/ffmpeg/ffmpeg_demuxer.h"
#include "backends/ffmpeg/ffmpeg_subtitle_decoder.h"
#include "backends/ffmpeg/ffmpeg_video_decoder.h"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>

#include "backends/apple/apple_frame_converter.h"
#include "backends/apple/core_audio_sink.h"
#include "backends/apple/vt_video_decoder.h"
#include "media/probing_demuxer.h"
#if !defined(LYCHEE_WITH_FFMPEG)
#include "backends/apple/at_audio_decoder.h"
#include "media/text_subtitle_decoder.h"
#endif
#elif defined(__ANDROID__)
#include <android/native_window.h>

#include "backends/android/aaudio_sink.h"
#include "backends/android/media_codec_video_decoder.h"
#include "media/probing_demuxer.h"
#if !defined(LYCHEE_WITH_FFMPEG)
#include "backends/android/media_codec_audio_decoder.h"
#include "media/text_subtitle_decoder.h"
#endif
#elif defined(_WIN32)
#include <unknwn.h>

#include "backends/media_foundation/mf_video_decoder.h"
#include "backends/windows/wasapi_sink.h"
#include "media/probing_demuxer.h"
#if !defined(LYCHEE_WITH_FFMPEG)
#include "backends/media_foundation/mf_audio_decoder.h"
#include "media/text_subtitle_decoder.h"
#endif
#elif defined(__linux__)
#include "backends/linux/pulse_audio_sink.h"
#if defined(LYCHEE_WITH_FFMPEG)
#include "media/probing_demuxer.h"
#endif
#endif

namespace lychee {

namespace {

std::atomic<bool> g_null_audio_for_testing{false};

}  // namespace

std::unique_ptr<Demuxer> CreateDemuxer() {
#if defined(LYCHEE_WITH_FFMPEG) && \
    (defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__))
  // MPEG-TS and HLS through the own demuxers, which put timestamps that
  // start over (in a file, at HLS discontinuities, across audio
  // renditions) on one timeline, and seek HLS by segment; FFmpeg's
  // passes them on as they are. FFmpeg takes the rest and what those
  // cannot play. Media Foundation and MediaCodec take TS video without
  // extradata (Annex B, parameter sets in band).
  return std::make_unique<ProbingDemuxer>(
      [](std::unique_ptr<DataSource> probed) -> std::unique_ptr<Demuxer> {
        return std::make_unique<FFmpegDemuxer>(std::move(probed));
      });
#elif defined(LYCHEE_WITH_FFMPEG) && defined(__linux__) && !defined(__ANDROID__)
  // The same as on Apple; FFmpegDemuxer opens the URL anew, since it
  // reads through FFmpeg's own protocols here (ffmpeg_demuxer.h).
  return std::make_unique<ProbingDemuxer>(
      [](std::unique_ptr<DataSource>) -> std::unique_ptr<Demuxer> {
        return std::make_unique<FFmpegDemuxer>();
      });
#elif defined(LYCHEE_WITH_FFMPEG)
  return std::make_unique<FFmpegDemuxer>();
#elif defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__)
#if defined(__ANDROID__)
  if (const auto* backend = AndroidFFmpegExtension().backend()) {
    return std::make_unique<ProbingDemuxer>(backend->demuxer);
  }
#endif
  return std::make_unique<ProbingDemuxer>();
#else
  return nullptr;
#endif
}

std::vector<std::unique_ptr<VideoDecoder>> CreateVideoDecoders(
    [[maybe_unused]] bool allow_hardware,
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
    if (!AndroidFFmpegExtension().backend()) {
      decoders.push_back(std::make_unique<MediaCodecVideoDecoder>(
          surface, rotate_surface_frames,
          MediaCodecVideoDecoder::Codecs::kSoftwareOnly));
    }
  }
  if (const auto* backend = AndroidFFmpegExtension().backend()) {
    if (allow_hardware) {
      decoders.push_back(backend->video(true, accepted_storage, device));
    }
    decoders.push_back(backend->video(false, accepted_storage, device));
  }
#endif
#if defined(__APPLE__)
#if TARGET_OS_OSX || defined(LYCHEE_WITH_FFMPEG)
  // FFmpeg's VideoToolbox adapter waits after every frame. The native
  // decoder batches file input; FFmpeg remains the fallback for streams
  // it cannot decode.
  if (allow_hardware) {
    decoders.push_back(std::make_unique<VTVideoDecoder>(true));
  }
#if !defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<VTVideoDecoder>(false));
#endif
#else
  // iOS lets apps pick neither: VideoToolbox decides.
  decoders.push_back(std::make_unique<VTVideoDecoder>(true));
#endif
#endif
#if defined(_WIN32)
  // In both builds, ahead of FFmpeg's D3D11VA, which stays the fallback
  // for what Windows' decoders refuse (HEVC without the Store's HEVC
  // extension, VP9, a GPU without the profile).
  if (allow_hardware) {
    decoders.push_back(
        std::make_unique<MFVideoDecoder>(accepted_storage, device, true));
  }
#if !defined(LYCHEE_WITH_FFMPEG)
  // Without FFmpeg, the same decoders on the CPU: no GPU, or one that lacks
  // the codec or the profile.
  decoders.push_back(
      std::make_unique<MFVideoDecoder>(accepted_storage, device, false));
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
  if (const auto* backend = AndroidFFmpegExtension().backend()) {
    decoders.push_back(backend->audio());
    return decoders;
  }
  decoders.push_back(std::make_unique<MediaCodecAudioDecoder>());
#elif defined(_WIN32) && !defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<MFAudioDecoder>());
#endif
#if defined(LYCHEE_WITH_FFMPEG)
  decoders.push_back(std::make_unique<FFmpegAudioDecoder>());
#endif
  return decoders;
}

namespace {

// Of several decoders' answers: yes if one says yes, else unknown if one
// cannot tell.
[[maybe_unused]] void Merge(int answer, int* result) {
  if (answer == 1 || (answer == -1 && *result == 0)) {
    *result = answer;
  }
}

}  // namespace

int VideoCodecSupported(VideoCodec codec, [[maybe_unused]] bool hardware) {
  if (codec != VideoCodec::kH264 && codec != VideoCodec::kHEVC) {
    return 0;
  }
  int result = 0;
#if defined(__APPLE__)
  // Apple's software decoder takes both where hardware lacks one (on iOS
  // VideoToolbox chooses, CreateVideoDecoders()).
  Merge(hardware ? (VTVideoDecoder::HardwareDecodes(codec) ? 1 : 0) : 1,
        &result);
#elif defined(__ANDROID__)
  // MediaCodec lists its codecs only through Java (MediaCodecList), and
  // the FFmpeg extension's build is not known here.
  Merge(-1, &result);
#elif defined(_WIN32)
  // Whether a GPU decodes it depends on the adapter and the stream's
  // profile, found only when one opens.
  Merge(hardware ? -1 : (MFVideoDecoder::CanDecode(codec) ? 1 : 0), &result);
#endif
#if defined(LYCHEE_WITH_FFMPEG)
  // FFmpeg's hardware decoding (VAAPI, D3D11VA, MediaCodec) is found only
  // by opening a device.
  if (FFmpegDecodes(codec)) {
    Merge(hardware ? -1 : 1, &result);
  }
#endif
  return result;
}

int AudioCodecSupported(AudioCodec codec) {
  if (codec != AudioCodec::kAAC && codec != AudioCodec::kALAC) {
    return 0;
  }
  int result = 0;
#if defined(__APPLE__) && !defined(LYCHEE_WITH_FFMPEG)
  // AudioToolbox has both.
  Merge(1, &result);
#elif defined(__ANDROID__) && !defined(LYCHEE_WITH_FFMPEG)
  // Android requires an AAC decoder; ALAC is up to the device, or the
  // FFmpeg extension.
  Merge(codec == AudioCodec::kAAC ? 1 : -1, &result);
#elif defined(_WIN32) && !defined(LYCHEE_WITH_FFMPEG)
  Merge(MFAudioDecoder::CanDecode(codec) ? 1 : 0, &result);
#endif
#if defined(LYCHEE_WITH_FFMPEG)
  Merge(FFmpegDecodes(codec) ? 1 : 0, &result);
#endif
  return result;
}

std::unique_ptr<SubtitleDecoder> CreateSubtitleDecoder() {
#if defined(LYCHEE_WITH_FFMPEG)
  return std::make_unique<FFmpegSubtitleDecoder>();
#elif defined(__APPLE__) || defined(_WIN32) || defined(__ANDROID__)
#if defined(__ANDROID__)
  if (const auto* backend = AndroidFFmpegExtension().backend()) {
    return backend->subtitle();
  }
#endif
  return std::make_unique<TextSubtitleDecoder>();
#else
  return nullptr;
#endif
}

std::unique_ptr<AudioSink> CreateAudioSink() {
  if (IsVirtualTime() || g_null_audio_for_testing.load()) {
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

void UseNullAudioSinkForTesting() {
  g_null_audio_for_testing = true;
}

std::unique_ptr<FrameConverter> CreateFrameConverter() {
#if defined(__APPLE__)
  return std::make_unique<AppleFrameConverter>();
#else
  return std::make_unique<CpuFrameConverter>();
#endif
}

}  // namespace lychee
