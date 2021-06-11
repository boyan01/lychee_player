//
// Created by boyan on 2021/2/16.
//

#include <iostream>
#include <exception>

#include <android/log.h>

#include "media_player_plugin.h"
#include "flutter_texture_entry.h"

#include <jni.h>
#include <jni.h>

#include <media_api.h>
#include <android/native_window_jni.h>

namespace {

JavaVM *g_vm;
jclass bridge_class;

class AndroidMediaTexture : public FlutterMediaTexture {

 public:

  explicit AndroidMediaTexture(std::unique_ptr<media::FlutterTextureEntry> texture)
      : texture_(std::move(texture)), window_buffer_() {
  }

  ~AndroidMediaTexture() override = default;

  int64_t GetTextureId() override {
    return texture_->id();
  }
  void Release() override {
    texture_->Release();
  }
  void MaybeInitPixelBuffer(int width, int height) override {

  }

  int GetWidth() override {
    return window_buffer_.width;
  }

  int GetHeight() override {
    return window_buffer_.height;
  }

  PixelFormat GetSupportFormat() override {
    return kFormat_32_RGBA;
  }

  void LockBuffer() override {
    auto ret = ANativeWindow_lock(texture_->native_window(), &window_buffer_, nullptr);
    if (ret < 0) {
      __android_log_print(ANDROID_LOG_INFO, "MediaPlayerPlugin", "LockBuffer: %d", ret);
    }
  }

  void UnlockBuffer() override {
    ANativeWindow_unlockAndPost(texture_->native_window());
  }

  void NotifyBufferUpdate() override {

  }

  uint8_t *GetBuffer() override {
    return static_cast<uint8_t *>(window_buffer_.bits);
  }

 private:
  ANativeWindow_Buffer window_buffer_;
  std::unique_ptr<media::FlutterTextureEntry> texture_;

};

void flutter_texture_factory(std::function<void(std::unique_ptr<FlutterMediaTexture>)> callback) {
  JNIEnv *g_env;
  if (g_vm->AttachCurrentThread(&g_env, nullptr) != 0) {
    std::cout << "Failed to attach" << std::endl;
    callback(nullptr);
    return;
  }
  jmethodID register_method_id = g_env->GetStaticMethodID(bridge_class,
                                                          "register",
                                                          "()Lcom/github/boyan01/lychee/FlutterTexture;");
  jobject j_texture_obj = g_env->CallStaticObjectMethod(bridge_class, register_method_id);

  callback(
      std::make_unique<AndroidMediaTexture>(
          std::make_unique<media::FlutterTextureEntry>(
              g_env,
              j_texture_obj
          )
      )
  );
}

}

jint JNI_OnLoad(JavaVM *vm, void * /*reserved*/) {
  g_vm = vm;
  return JNI_VERSION_1_4;
}

void JNI_OnUnload(JavaVM *vm, void * /*reserved*/) {
  g_vm = nullptr;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_github_boyan01_lychee_MediaPlayerBridge_setupJNI(JNIEnv *env, jclass clazz) {
  __android_log_print(ANDROID_LOG_INFO, "MediaPlayerPlugin", "MediaPlayerBridge_SetupJNI");
  bridge_class = reinterpret_cast<jclass>(env->NewGlobalRef(clazz));
  register_flutter_texture_factory(flutter_texture_factory);
}

