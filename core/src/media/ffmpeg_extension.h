#ifndef LYCHEE_MEDIA_FFMPEG_EXTENSION_H_
#define LYCHEE_MEDIA_FFMPEG_EXTENSION_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "base/log.h"
#include "media/audio_decoder.h"
#include "media/data_source.h"
#include "media/demuxer.h"
#include "media/subtitle_decoder.h"
#include "media/video_decoder.h"
#include "media/video_device.h"

namespace lychee {

// Private to the Android SDK, never installed with the C ABI. C++ objects
// cross this seam only after the descriptor's build fingerprint matches:
// both AARs must use the same core sources, SDK version and NDK toolchain.
// The extension owns FFmpeg; these callbacks keep HTTP and logging in the
// one core, including its Java VM, network policy and user's log sink.
struct FFmpegHost {
  decltype(&OpenDataSource) open_data_source;
  decltype(&CreateMemoryDataSource) memory_data_source;
  bool (*http_available)();
  std::string (*resolve_url)(const std::string&, const std::string&);
  decltype(&UnpackSubtitleCue) unpack_subtitle;
  decltype(&GetLogLevel) log_level;
  decltype(&LogMessage) log;
};

struct FFmpegBackend {
  std::unique_ptr<Demuxer> (*demuxer)(std::unique_ptr<DataSource>);
  std::unique_ptr<VideoDecoder> (*video)(bool, uint32_t, const VideoDevice&);
  std::unique_ptr<AudioDecoder> (*audio)();
  std::unique_ptr<SubtitleDecoder> (*subtitle)();
};

// Only this fixed C layout is inspected before the compatibility check.
// Increase the symbol's suffix if the descriptor itself ever changes.
struct FFmpegExtensionDescriptor {
  uint32_t struct_size;
  const char* build_id;
  const FFmpegBackend* (*initialize)(const FFmpegHost*);
};

class FFmpegExtension {
 public:
  bool Load(const char* library, std::string* error);
  void Seal();
  const FFmpegBackend* backend() const { return backend_.load(); }

 private:
  std::mutex mutex_;
  bool sealed_ = false;
  std::atomic<const FFmpegBackend*> backend_{nullptr};
  // Successful loads stay resident for the process lifetime: packets,
  // codec parameters and frames can outlive the decoder that produced them.
};

FFmpegExtension& AndroidFFmpegExtension();

}  // namespace lychee

#endif  // LYCHEE_MEDIA_FFMPEG_EXTENSION_H_
