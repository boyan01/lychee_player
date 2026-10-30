#include "media/subtitle_image.h"

#include <android/bitmap.h>
#include <android/imagedecoder.h>
#include <dlfcn.h>

#include <cstring>

#include "backends/android/jvm.h"

namespace lychee {
namespace {
bool DecodeJava(std::span<const uint8_t> bytes, SubtitleBitmap* image) {
  JNIEnv* env = AttachedEnv();
  if (!env) {
    return false;
  }
  LocalFrame locals(env);
  jclass factory = env->FindClass("android/graphics/BitmapFactory");
  if (!factory || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  jmethodID decode = env->GetStaticMethodID(factory, "decodeByteArray",
                                            "([BII)Landroid/graphics/Bitmap;");
  if (!decode || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  jbyteArray data = env->NewByteArray(static_cast<jsize>(bytes.size()));
  if (!data || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  env->SetByteArrayRegion(data, 0, static_cast<jsize>(bytes.size()),
                          reinterpret_cast<const jbyte*>(bytes.data()));
  if (env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  jobject bitmap = env->CallStaticObjectMethod(factory, decode, data, 0,
                                               static_cast<jint>(bytes.size()));
  if (!bitmap || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  AndroidBitmapInfo info = {};
  void* pixels = nullptr;
  const bool locked = AndroidBitmap_getInfo(env, bitmap, &info) ==
                          ANDROID_BITMAP_RESULT_SUCCESS &&
                      info.format == ANDROID_BITMAP_FORMAT_RGBA_8888 &&
                      info.width == static_cast<uint32_t>(image->width) &&
                      info.height == static_cast<uint32_t>(image->height) &&
                      AndroidBitmap_lockPixels(env, bitmap, &pixels) ==
                          ANDROID_BITMAP_RESULT_SUCCESS;
  if (locked) {
    for (int y = 0; y < image->height; ++y) {
      std::memcpy(
          image->rgba.data() + static_cast<size_t>(y) * image->width * 4,
          static_cast<const uint8_t*>(pixels) +
              static_cast<size_t>(y) * info.stride,
          static_cast<size_t>(image->width) * 4);
    }
    AndroidBitmap_unlockPixels(env, bitmap);
  }
  const bool clean = TakeException(env).empty();
  jclass cls = env->GetObjectClass(bitmap);
  jmethodID recycle = cls ? env->GetMethodID(cls, "recycle", "()V") : nullptr;
  if (recycle) {
    env->CallVoidMethod(bitmap, recycle);
  }
  return TakeException(env).empty() && clean && locked;
}

// API 30's native decoder also serves the CLI without a Java VM. Resolve
// dynamically so SDK applications keep their API 26 minimum and BitmapFactory.
bool DecodeNative(std::span<const uint8_t> bytes, SubtitleBitmap* image) {
  void* library = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    return false;
  }
  using Create = int (*)(const void*, size_t, AImageDecoder**);
  using SetFormat = int (*)(AImageDecoder*, int32_t);
  using Decode = int (*)(AImageDecoder*, void*, size_t, size_t);
  using Destroy = void (*)(AImageDecoder*);
  auto create = reinterpret_cast<Create>(
      dlsym(library, "AImageDecoder_createFromBuffer"));
  auto set_format = reinterpret_cast<SetFormat>(
      dlsym(library, "AImageDecoder_setAndroidBitmapFormat"));
  auto decode =
      reinterpret_cast<Decode>(dlsym(library, "AImageDecoder_decodeImage"));
  auto destroy =
      reinterpret_cast<Destroy>(dlsym(library, "AImageDecoder_delete"));
  AImageDecoder* decoder = nullptr;
  const bool ok =
      create && set_format && decode && destroy &&
      create(bytes.data(), bytes.size(), &decoder) ==
          ANDROID_IMAGE_DECODER_SUCCESS &&
      set_format(decoder, ANDROID_BITMAP_FORMAT_RGBA_8888) ==
          ANDROID_IMAGE_DECODER_SUCCESS &&
      decode(decoder, image->rgba.data(), static_cast<size_t>(image->width) * 4,
             image->rgba.size()) == ANDROID_IMAGE_DECODER_SUCCESS;
  if (decoder) {
    destroy(decoder);
  }
  dlclose(library);
  return ok;
}
}  // namespace

bool DecodeSubtitlePng(std::span<const uint8_t> bytes, SubtitleBitmap* image) {
  SubtitleBitmap bitmap;
  if (!SubtitlePngSize(bytes, &bitmap.width, &bitmap.height)) {
    return false;
  }
  bitmap.rgba.resize(static_cast<size_t>(bitmap.width) * bitmap.height * 4);
  if (!DecodeJava(bytes, &bitmap) && !DecodeNative(bytes, &bitmap)) {
    return false;
  }
  *image = std::move(bitmap);
  return true;
}
}  // namespace lychee
