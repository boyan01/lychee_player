#include "media/ffmpeg_extension.h"

#include <dlfcn.h>
#include <jni.h>

#include <cstring>

#include "backends/android/http_data_source.h"
#include "backends/hls/hls_playlist.h"
#include "ffmpeg_extension_build.h"

namespace lychee {

bool FFmpegExtension::Load(const char* library, std::string* error) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (backend_.load()) {
    return true;
  }
  if (sealed_) {
    *error = "Call LycheeFFmpeg.load() before creating the first player";
    return false;
  }
  void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    *error = "Cannot load the lychee-ffmpeg extension: ";
    *error += dlerror();
    return false;
  }
  using Entry = const FFmpegExtensionDescriptor* (*)();
  auto entry =
      reinterpret_cast<Entry>(dlsym(handle, "lychee_ffmpeg_extension_v1"));
  const auto* descriptor = entry ? entry() : nullptr;
  if (!descriptor || descriptor->struct_size != sizeof(*descriptor) ||
      !descriptor->build_id ||
      strcmp(descriptor->build_id, LYCHEE_FFMPEG_EXTENSION_BUILD) != 0 ||
      !descriptor->initialize) {
    *error =
        "Incompatible lychee-ffmpeg extension; use matching AARs from the same "
        "SDK build";
    dlclose(handle);
    return false;
  }
  static const FFmpegHost host{
      OpenDataSource, CreateMemoryDataSource, HttpDataSourceAvailable,
      ResolveUrl,     UnpackSubtitleCue,      GetLogLevel,
      LogMessage};
  const auto* backend = descriptor->initialize(&host);
  if (!backend || !backend->demuxer || !backend->video || !backend->audio ||
      !backend->subtitle) {
    *error = "Invalid lychee-ffmpeg backend";
    dlclose(handle);
    return false;
  }
  backend_.store(backend);
  return true;
}

void FFmpegExtension::Seal() {
  std::lock_guard<std::mutex> lock(mutex_);
  sealed_ = true;
}

FFmpegExtension& AndroidFFmpegExtension() {
  static FFmpegExtension extension;
  return extension;
}

}  // namespace lychee

extern "C" JNIEXPORT void JNICALL
Java_tech_soit_lychee_LycheeFFmpeg_nativeLoad(JNIEnv* env, jobject) {
  std::string error;
#if defined(LYCHEE_WITH_FFMPEG)
  error = "This core already embeds FFmpeg; use the FFmpeg-free lychee AAR";
#else
  if (lychee::AndroidFFmpegExtension().Load("liblychee_ffmpeg.so", &error)) {
    return;
  }
#endif
  env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),
                error.c_str());
}
