#include "media/ffmpeg_extension.h"

#include <cstdarg>
#include <cstdio>
#include <utility>

#include "backends/ffmpeg/ffmpeg_audio_decoder.h"
#include "backends/ffmpeg/ffmpeg_demuxer.h"
#include "backends/ffmpeg/ffmpeg_subtitle_decoder.h"
#include "backends/ffmpeg/ffmpeg_video_decoder.h"
#include "ffmpeg_extension_build.h"

namespace lychee {
namespace {

const FFmpegHost* host = nullptr;

const FFmpegBackend* Initialize(const FFmpegHost* callbacks) {
  host = callbacks;
  static const FFmpegBackend backend{
      [](std::unique_ptr<DataSource> source) -> std::unique_ptr<Demuxer> {
        return std::make_unique<FFmpegDemuxer>(std::move(source));
      },
      [](bool hardware, uint32_t storage,
         const VideoDevice& device) -> std::unique_ptr<VideoDecoder> {
        return std::make_unique<FFmpegVideoDecoder>(hardware, storage, device);
      },
      []() -> std::unique_ptr<AudioDecoder> {
        return std::make_unique<FFmpegAudioDecoder>();
      },
      []() -> std::unique_ptr<SubtitleDecoder> {
        return std::make_unique<FFmpegSubtitleDecoder>();
      }};
  return &backend;
}

}  // namespace

// Private forwarding stubs: no duplicate HTTP client, JVM or logging state,
// and no need to export the core's C++ implementation symbols.
std::unique_ptr<DataSource> OpenDataSource(const std::string& url,
                                           std::string* error) {
  return host->open_data_source(url, error);
}

bool HttpDataSourceAvailable() {
  return host->http_available();
}

std::unique_ptr<DataSource> CreateMemoryDataSource(std::vector<uint8_t> bytes) {
  return host->memory_data_source(std::move(bytes));
}

std::string ResolveUrl(const std::string& base, const std::string& relative) {
  return host->resolve_url(base, relative);
}

bool UnpackSubtitleCue(const EncodedPacket& packet, SubtitleCue* cue) {
  return host->unpack_subtitle(packet, cue);
}

LogLevel GetLogLevel() {
  return host->log_level();
}

void LogMessage(LogLevel level, const char* tag, const char* format, ...) {
  char message[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  host->log(level, tag, "%s", message);
}

}  // namespace lychee

extern "C" __attribute__((visibility("default")))
const lychee::FFmpegExtensionDescriptor*
lychee_ffmpeg_extension_v1() {
  static const lychee::FFmpegExtensionDescriptor descriptor{
      sizeof(descriptor), LYCHEE_FFMPEG_EXTENSION_BUILD, lychee::Initialize};
  return &descriptor;
}
