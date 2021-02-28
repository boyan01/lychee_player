//
// Created by boyan on 2021/2/17.
//
#include <android/native_window_jni.h>
#include "ffp_flutter_android.h"

extern "C" {
#include <libavutil/imgutils.h>
};

namespace media {

FlutterAndroidVideoRender::TextureRegistry FlutterAndroidVideoRender::flutter_texture_registry;

FlutterAndroidVideoRender::FlutterAndroidVideoRender()
    : window_buffer_(), img_convert_ctx_(nullptr) {
}

FlutterAndroidVideoRender::~FlutterAndroidVideoRender() {
  sws_freeContext(img_convert_ctx_);
  Detach();
}

void FlutterAndroidVideoRender::RenderPicture(Frame &frame) {
  CHECK_VALUE(texture_);
  CHECK_VALUE(texture_->native_window());

  if (ANativeWindow_lock(texture_->native_window(), &window_buffer_, nullptr) < 0) {
    av_log(nullptr, AV_LOG_WARNING, "FlutterAndroidVideoRender lock window failed.\n");
    return;
  }

  img_convert_ctx_ = sws_getCachedContext(img_convert_ctx_, frame.width, frame.height, AVPixelFormat(frame.format),
                                          window_buffer_.width, window_buffer_.height, AV_PIX_FMT_RGBA,
                                          SWS_BICUBIC,
                                          nullptr, nullptr, nullptr);
  if (!img_convert_ctx_) {
    av_log(nullptr, AV_LOG_ERROR, "can not init image convert context\n");
    ANativeWindow_unlockAndPost(texture_->native_window());
    return;
  }

#if 1
  auto *av_frame = frame.frame;
  int linesize[4] = {4 * window_buffer_.stride};
  uint8_t *bgr_buffer[8] = {static_cast<uint8_t *>(window_buffer_.bits)};
  sws_scale(img_convert_ctx_, av_frame->data, av_frame->linesize, 0, av_frame->height, bgr_buffer, linesize);

  av_log(nullptr, AV_LOG_INFO, "RenderPicture: %d, %d line size = %d\n",
         window_buffer_.width, window_buffer_.height, linesize[0]);
#endif

#if 1
  auto *buffer = static_cast<uint8_t *>(window_buffer_.bits);
  av_log(nullptr, AV_LOG_INFO, "render_frame(%lld, %p): render first pixel = 0x%2x%2x%2x%2x \n",
         texture_->id(), texture_.get(),
         buffer[3], buffer[0], buffer[1], buffer[2]);
#endif

  ANativeWindow_unlockAndPost(texture_->native_window());
}

int64_t FlutterAndroidVideoRender::Attach() {
  CHECK_VALUE_WITH_RETURN(!texture_, -1);
  CHECK_VALUE_WITH_RETURN(flutter_texture_registry, -1);
  this->texture_ = flutter_texture_registry();
  StartRenderThread();
  return this->texture_->id();
}

void FlutterAndroidVideoRender::Detach() {
  CHECK_VALUE(texture_);
  StopRenderThread();
  texture_->Release();
  texture_.reset(nullptr);
}

FlutterTextureEntry::FlutterTextureEntry(JNIEnv *env, jobject texture)
    : j_texture_entry_(texture), env(env) {
  CHECK_VALUE(texture);
  CHECK_VALUE(env);
  jclass clazz = env->GetObjectClass(texture);
  release_ = env->GetMethodID(clazz, "release", "()V");
  id_ = env->CallLongMethod(texture, env->GetMethodID(clazz, "getId", "()J"));
  jmethodID get_surface = env->GetMethodID(clazz, "getSurface", "()Landroid/view/Surface;");
  jobject surface = env->CallObjectMethod(texture, get_surface);
  CHECK_VALUE(surface);
  native_window_ = ANativeWindow_fromSurface(env, surface);
  CHECK_VALUE(native_window_);
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