//
// Created by yangbin on 2021/5/8.
//

#include "windows_video_render_sink.h"

#include "base/logging.h"

static flutter::TextureRegistrar *texture_registrar_;

void register_flutter_plugin(flutter::TextureRegistrar *texture_registrar) {
  DLOG(INFO) << "Windows register_flutter_plugin";
  DCHECK(texture_registrar);
  texture_registrar_ = texture_registrar;
}

namespace media {

int64_t WindowsVideoRenderSink::Attach() {
  DCHECK_LT(texture_id_, 0);
  DCHECK(texture_registrar_);
  {
    pixel_buffer_ = new FlutterDesktopPixelBuffer;
    pixel_buffer_->width = 0;
    pixel_buffer_->height = 0;
    pixel_buffer_->buffer = nullptr;
  }

  auto texture_variant = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
      [&](size_t width, size_t height) -> const FlutterDesktopPixelBuffer * {
        return pixel_buffer_;
      }));
  texture_id_ = texture_registrar_->RegisterTexture(texture_variant.get());
  if (texture_id_ < 0) {
    texture_registrar_->UnregisterTexture(texture_id_);
    return -1;
  }
  texture_ = std::move(texture_variant);
  return texture_id_;
}

void WindowsVideoRenderSink::Detach() {
  DCHECK_GE(texture_id_, 0);
  texture_registrar_->UnregisterTexture(texture_id_);
  texture_id_ = -1;
  if (pixel_buffer_) {
    delete[] pixel_buffer_->buffer;
    delete pixel_buffer_;
  }
  sws_freeContext(img_convert_ctx_);
}

void WindowsVideoRenderSink::DoRender(std::shared_ptr<VideoFrame> frame) {
  if (!pixel_buffer_ || frame->IsEmpty()) {
    return;
  }
  if (!pixel_buffer_->buffer) {
    DLOG(INFO) << "render_frame：init";
    pixel_buffer_->height = frame->Height();
    pixel_buffer_->width = frame->Width();
    pixel_buffer_->buffer = new uint8_t[frame->Height() * frame->Width() * 4];
  }
  auto av_frame = frame->frame();

  img_convert_ctx_ = sws_getCachedContext(img_convert_ctx_, av_frame->width, av_frame->height,
                                          AVPixelFormat(av_frame->format), av_frame->width,
                                          av_frame->height, AV_PIX_FMT_RGBA,
                                          NULL, nullptr, nullptr, nullptr);
  if (!img_convert_ctx_) {
    av_log(nullptr, AV_LOG_FATAL, "Can not initialize the conversion context\n");
    return;
  }
  int linesize[4] = {frame->Width() * 4};
//    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_ARGB, pFrame->width, pFrame->height, 1);
  uint8_t *bgr_buffer[8] = {(uint8_t *) pixel_buffer_->buffer};
  sws_scale(img_convert_ctx_, av_frame->data, av_frame->linesize, 0, av_frame->height, bgr_buffer, linesize);
#if 0
  av_log(nullptr, AV_LOG_INFO, "render_frame(%lld, %p): render first pixel = 0x%2x%2x%2x%2x \n",
         texture_id_, texture_.get(),
         pixel_buffer_->buffer[3], pixel_buffer_->buffer[0], pixel_buffer_->buffer[1], pixel_buffer_->buffer[2]);
#endif
  texture_registrar_->MarkTextureFrameAvailable(texture_id_);
}

}