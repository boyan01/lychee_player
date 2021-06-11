//
// Created by yangbin on 2021/5/6.
//

#ifndef MEDIA_FLUTTER_ANDROID_TEXTURE_REGISTRY_H_
#define MEDIA_FLUTTER_ANDROID_TEXTURE_REGISTRY_H_

#include "jni.h"

#include "android/log.h"
#include "android/native_window.h"

#include "base/basictypes.h"

namespace media {

class FlutterTextureEntry {

 private:
  JNIEnv *jni_env_;

  jobject j_texture_entry_;

  jmethodID release_;
  int64_t id_;

  ANativeWindow *native_window_;

  DELETE_COPY_AND_ASSIGN(FlutterTextureEntry);

 public:

  FlutterTextureEntry(JNIEnv *env, jobject texture);

  ~FlutterTextureEntry();

  inline int64_t id() {
    return id_;
  }

  ANativeWindow *native_window() const {
    return native_window_;
  };

};

}

#endif //MEDIA_FLUTTER_ANDROID_TEXTURE_REGISTRY_H_
