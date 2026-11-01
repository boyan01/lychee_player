#include "media/text_encoding.h"

#include "backends/android/jvm.h"

namespace lychee {

// Bionic's iconv knows only the UTF encodings and Latin-1; Java knows the
// rest.
bool ConvertToUtf8(std::string_view data,
                   const std::string& encoding,
                   std::string* text) {
  JNIEnv* env = AttachedEnv();
  if (!env) {
    return false;
  }
  LocalFrame locals(env);
  jbyteArray bytes = env->NewByteArray(static_cast<jsize>(data.size()));
  jstring name = env->NewStringUTF(encoding.c_str());
  jstring utf8 = env->NewStringUTF("UTF-8");
  jclass string_class = env->FindClass("java/lang/String");
  if (!bytes || !name || !utf8 || !string_class || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()),
                          reinterpret_cast<const jbyte*>(data.data()));
  jmethodID create =
      env->GetMethodID(string_class, "<init>", "([BLjava/lang/String;)V");
  jmethodID get_bytes =
      env->GetMethodID(string_class, "getBytes", "(Ljava/lang/String;)[B");
  if (!create || !get_bytes || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  // Throws UnsupportedEncodingException for a name Java does not know;
  // replaces what the encoding cannot read with U+FFFD.
  jobject decoded = env->NewObject(string_class, create, bytes, name);
  if (!decoded || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  // Not GetStringUTFChars: Java's modified UTF-8 splits characters past
  // U+FFFF into surrogates.
  auto encoded = static_cast<jbyteArray>(
      env->CallObjectMethod(decoded, get_bytes, utf8));
  if (!encoded || env->ExceptionCheck()) {
    TakeException(env);
    return false;
  }
  const jsize size = env->GetArrayLength(encoded);
  text->resize(static_cast<size_t>(size));
  env->GetByteArrayRegion(encoded, 0, size,
                          reinterpret_cast<jbyte*>(text->data()));
  return TakeException(env).empty();
}

}  // namespace lychee
