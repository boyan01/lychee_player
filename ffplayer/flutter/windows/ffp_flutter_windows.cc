//
// Created by boyan on 2021/1/24.
//

#include <list>
#include <iostream>
#include <memory>

#include "flutter_texture_registrar.h"

extern "C" {
#include "libswscale/swscale.h"
}

#include "ffp_flutter_windows.h"

static flutter::TextureRegistrar *texture_registrar;

void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar) {
  texture_registrar = registrar->texture_registrar();
  std::cout << "register_flutter_plugin: " << registrar << std::endl;
}

void FlutterWindowsVideoRender::RenderPicture(Frame &frame) {
  if (!pixel_buffer_->buffer) {
    std::cout << "render_frame：init" << std::endl;
    pixel_buffer_->height = frame.height;
    pixel_buffer_->width = frame.width;
    pixel_buffer_->buffer = new uint8_t[frame.height * frame.width * 4];
  }
  auto av_frame = frame.frame;

  img_convert_ctx_ = sws_getCachedContext(img_convert_ctx_, av_frame->width, av_frame->height,
                                          AVPixelFormat(av_frame->format), av_frame->width,
                                          av_frame->height, AV_PIX_FMT_RGBA,
                                          NULL, nullptr, nullptr, nullptr);
  if (!img_convert_ctx_) {
    av_log(nullptr, AV_LOG_FATAL, "Can not initialize the conversion context\n");
    return;
  }
  int linesize[4] = {frame.width * 4};
//    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_ARGB, pFrame->width, pFrame->height, 1);
  uint8_t *bgr_buffer[8] = {(uint8_t *) pixel_buffer_->buffer};
  sws_scale(img_convert_ctx_, av_frame->data, av_frame->linesize, 0, av_frame->height, bgr_buffer, linesize);
#if 0
  av_log(nullptr, AV_LOG_INFO, "render_frame(%lld, %p): render first pixel = 0x%2x%2x%2x%2x \n",
         texture_id_, texture_.get(),
         pixel_buffer_->buffer[3], pixel_buffer_->buffer[0], pixel_buffer_->buffer[1], pixel_buffer_->buffer[2]);
#endif
  texture_registrar->MarkTextureFrameAvailable(texture_id_);
}

FlutterWindowsVideoRender::~FlutterWindowsVideoRender() {
  Detach();
}

int64_t FlutterWindowsVideoRender::Attach() {
  CHECK_VALUE_WITH_RETURN(texture_id_ < 0, -1);
  CHECK_VALUE_WITH_RETURN(texture_registrar, -1);
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
  texture_id_ = texture_registrar->RegisterTexture(texture_variant.get());
  if (texture_id_ < 0) {
    texture_registrar->UnregisterTexture(texture_id_);
    return -1;
  }
  texture_ = std::move(texture_variant);
  StartRenderThread();
  return texture_id_;
}

void FlutterWindowsVideoRender::Detach() {
  CHECK_VALUE(texture_id_ >= 0);
  StopRenderThread();
  texture_registrar->UnregisterTexture(texture_id_);
  texture_id_ = -1;
  delete pixel_buffer_->buffer;
  delete pixel_buffer_;
  sws_freeContext(img_convert_ctx_);
}
