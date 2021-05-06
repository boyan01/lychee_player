//
// Created by yangbin on 2021/5/6.
//

#include "android_video_renderer_sink.h"

#include "base/logging.h"

namespace media {

AndroidVideoRendererSink::TextureRegistry AndroidVideoRendererSink::flutter_texture_registry;

AndroidVideoRendererSink::AndroidVideoRendererSink()
    : FlutterVideoRendererSink(),
      window_buffer_(),
      img_convert_ctx_(nullptr) {

}

AndroidVideoRendererSink::~AndroidVideoRendererSink() {
  sws_freeContext(img_convert_ctx_);
}

void AndroidVideoRendererSink::DoRender(std::shared_ptr<VideoFrame> frame) {
  if (!texture_) {
    return;
  }

  DCHECK(texture_->native_window());

  if (ANativeWindow_lock(texture_->native_window(), &window_buffer_, nullptr) < 0) {
    av_log(nullptr, AV_LOG_WARNING, "FlutterAndroidVideoRender lock window failed.\n");
    return;
  }

  img_convert_ctx_ = sws_getCachedContext(
      img_convert_ctx_, frame->Width(), frame->Height(), AVPixelFormat(frame->frame()->format),
      window_buffer_.width, window_buffer_.height, AV_PIX_FMT_RGBA, SWS_BICUBIC,
      nullptr, nullptr, nullptr);
  if (!img_convert_ctx_) {
    av_log(nullptr, AV_LOG_ERROR, "can not init image convert context\n");
    ANativeWindow_unlockAndPost(texture_->native_window());
    return;
  }

#if 1
  auto *av_frame = frame->frame();
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

int64_t AndroidVideoRendererSink::Attach() {
  DCHECK(!texture_);
  DCHECK(flutter_texture_registry);

  this->texture_ = flutter_texture_registry();
  return this->texture_->id();
}

void AndroidVideoRendererSink::Detach() {
  DCHECK(texture_);
  texture_->Release();
  texture_.reset(nullptr);
}

}