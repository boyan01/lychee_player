//
// Created by yangbin on 2021/5/6.
//

#include "flutter_texture_entry.h"

#include "android/native_window_jni.h"

#include "base/logging.h"

namespace media {

FlutterTextureEntry::FlutterTextureEntry(JNIEnv *env, jobject texture)
    : j_texture_entry_(texture), env(env) {
  DCHECK(texture);
  DCHECK(env);
  jclass clazz = env->GetObjectClass(texture);
  release_ = env->GetMethodID(clazz, "release", "()V");
  id_ = env->CallLongMethod(texture, env->GetMethodID(clazz, "getId", "()J"));
  jmethodID get_surface = env->GetMethodID(clazz, "getSurface", "()Landroid/view/Surface;");
  jobject surface = env->CallObjectMethod(texture, get_surface);
  DCHECK(surface);
  native_window_ = ANativeWindow_fromSurface(env, surface);
  DCHECK(native_window_);
  ANativeWindow_setBuffersGeometry(native_window_, 1280, 720, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM);
}

FlutterTextureEntry::~FlutterTextureEntry() {
  if (!released_) {
    Release();
  }
}

void FlutterTextureEntry::Release() {
  if (released_) {
    return;
  }
  released_ = true;
  if (native_window_) {
    ANativeWindow_release(native_window_);
  }
  env->CallVoidMethod(j_texture_entry_, release_);
}

}