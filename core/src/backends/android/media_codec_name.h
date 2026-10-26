#ifndef LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_NAME_H_
#define LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_NAME_H_

#include <dlfcn.h>
#include <media/NdkMediaCodec.h>

#include <string>

namespace lychee {

// The codec's name (c2.android.aac.decoder, ...), or empty before API 28,
// whose symbols are resolved at runtime so the library loads on API 26.
inline std::string MediaCodecName(AMediaCodec* codec) {
  using GetName = media_status_t (*)(AMediaCodec*, char**);
  using ReleaseName = void (*)(AMediaCodec*, char*);
  static auto get_name =
      reinterpret_cast<GetName>(dlsym(RTLD_DEFAULT, "AMediaCodec_getName"));
  static auto release_name = reinterpret_cast<ReleaseName>(
      dlsym(RTLD_DEFAULT, "AMediaCodec_releaseName"));
  char* name = nullptr;
  if (!get_name || get_name(codec, &name) != AMEDIA_OK || !name) {
    return {};
  }
  std::string result = name;
  if (release_name) {
    release_name(codec, name);
  }
  return result;
}

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_NAME_H_
