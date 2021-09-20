//
// Created by boyan on 2021/2/16.
//

#include <iostream>
#include <exception>
#include <jni.h>

#include <android/log.h>
#include <android/native_window_jni.h>

#include "base/logging.h"

#include "media_player_plugin.h"
#include "flutter_texture_entry.h"

#include <external_media_texture.h>

JavaVM *g_vm;

namespace {

jclass bridge_class;

class AndroidMediaTexture : public ExternalMediaTexture {

 public:

  explicit AndroidMediaTexture(std::unique_ptr<media::FlutterTextureEntry> texture)
      : texture_(std::move(texture)), window_buffer_() {
  }

  ~AndroidMediaTexture() override {
    std::lock_guard<std::mutex> lock_guard(pixel_mutex_);
    texture_.reset(nullptr);
    DLOG(INFO) << "~AndroidMediaTexture";
  }

  int64_t GetTextureId() override {
    return texture_->id();
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

  bool TryLockBuffer() override {
    pixel_mutex_.lock();
    auto ret = ANativeWindow_lock(texture_->native_window(),
                                  &window_buffer_, nullptr);

    if (ret < 0) {
      pixel_mutex_.unlock();
      DLOG(ERROR) << "LockBuffer failed: " << ret;
      return false;
    }
    return true;
  }

  void UnlockBuffer() override {
    pixel_mutex_.unlock();
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

  std::mutex pixel_mutex_;

};

void flutter_texture_factory(std::function<void(std::unique_ptr<ExternalMediaTexture>)> callback) {
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

  callback(std::make_unique<AndroidMediaTexture>(
      std::make_unique<media::FlutterTextureEntry>(
          g_env,
          j_texture_obj
      ))
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
  register_external_texture_factory(flutter_texture_factory);
}

