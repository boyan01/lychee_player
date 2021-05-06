//
// Created by yangbin on 2021/5/6.
//

#ifndef MEDIA_FLUTTER_ANDROID_TEXTURE_REGISTRY_H_
#define MEDIA_FLUTTER_ANDROID_TEXTURE_REGISTRY_H_

#include "jni.h"

#include "android/log.h"
#include "android/native_window.h"

namespace media {

class FlutterTextureEntry {

 private:
  JNIEnv *env;

  jobject j_texture_entry_;

  jmethodID release_;
  int64_t id_;

  ANativeWindow *native_window_;

  bool released_ = false;

 public:

  FlutterTextureEntry(JNIEnv *env, jobject texture);

  ~FlutterTextureEntry();

  inline int64_t id() {
    return id_;
  }

  ANativeWindow *native_window() const { return native_window_; };

  void Release();

};

}

#endif //MEDIA_FLUTTER_ANDROID_TEXTURE_REGISTRY_H_
