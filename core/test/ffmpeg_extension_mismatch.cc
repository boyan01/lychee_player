#include "media/ffmpeg_extension.h"

#include <cstdlib>

// Deliberately not this build's fingerprint. The loader must reject it
// before Initialize can return or exchange any C++ object.
extern "C" __attribute__((visibility("default")))
const lychee::FFmpegExtensionDescriptor*
lychee_ffmpeg_extension_v1() {
  static const lychee::FFmpegExtensionDescriptor descriptor{
      sizeof(descriptor), "different-sdk-build",
      [](const lychee::FFmpegHost*) -> const lychee::FFmpegBackend* {
        std::abort();
      }};
  return &descriptor;
}
