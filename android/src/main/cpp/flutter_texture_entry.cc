//
// Created by yangbin on 2021/5/6.
//

#include "flutter_texture_entry.h"

#include "android/native_window_jni.h"

#include "base/logging.h"

#include "media_player_plugin.h"

namespace media {

FlutterTextureEntry::FlutterTextureEntry(JNIEnv *env, jobject texture)
    : j_texture_entry_(texture), jni_env_(env) {
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
  ANativeWindow_setBuffersGeometry(native_window_,
                                   1280,
                                   720,
                                   WINDOW_FORMAT_RGBA_8888);
}

FlutterTextureEntry::~FlutterTextureEntry() {
  DLOG(INFO) << "~FlutterTextureEntry";
  ANativeWindow_release(native_window_);
  jni_env_->CallVoidMethod(j_texture_entry_, release_);
  g_vm->DetachCurrentThread();
}

}