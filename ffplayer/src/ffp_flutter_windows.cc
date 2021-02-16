//
// Created by boyan on 2021/1/24.
//

#if defined(_FLUTTER) && defined(_FLUTTER_WINDOWS)

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
    if (!pixel_buffer->buffer) {
        std::cout << "render_frame：init" << std::endl;
        pixel_buffer->height = frame.height;
        pixel_buffer->width = frame.width;
        pixel_buffer->buffer = new uint8_t[frame.height * frame.width * 4];
    }
    auto pFrame = frame.frame;

    img_convert_ctx = sws_getCachedContext(img_convert_ctx, pFrame->width, pFrame->height,
                                           AVPixelFormat(pFrame->format), pFrame->width,
                                           pFrame->height, AV_PIX_FMT_RGBA,
                                           NULL, nullptr, nullptr, nullptr);
    if (!img_convert_ctx) {
        av_log(nullptr, AV_LOG_FATAL, "Can not initialize the conversion context\n");
        return;
    }
    int linesize[4] = {frame.width * 4};
//    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_ARGB, pFrame->width, pFrame->height, 1);
    uint8_t *bgr_buffer[8] = {(uint8_t *) pixel_buffer->buffer};
    sws_scale(img_convert_ctx, pFrame->data, pFrame->linesize, 0, pFrame->height, bgr_buffer, linesize);
#if 1
    av_log(nullptr, AV_LOG_INFO, "render_frame(%lld, %p): render first pixel = 0x%2x%2x%2x%2x \n",
           texture_id, texture_.get(),
           pixel_buffer->buffer[3], pixel_buffer->buffer[0], pixel_buffer->buffer[1], pixel_buffer->buffer[2]);
#endif
    texture_registrar->MarkTextureFrameAvailable(texture_id);
}

FlutterWindowsVideoRender::~FlutterWindowsVideoRender() {
    Detach();
}

int FlutterWindowsVideoRender::Attach() {
    CHECK_VALUE_WITH_RETURN(texture_id < 0, -1);
    CHECK_VALUE_WITH_RETURN(texture_registrar, -1);
    {
        pixel_buffer = new FlutterDesktopPixelBuffer;
        pixel_buffer->width = 0;
        pixel_buffer->height = 0;
        pixel_buffer->buffer = nullptr;
    }

    auto texture_variant = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [this](size_t width, size_t height) -> const FlutterDesktopPixelBuffer * {
              av_log(nullptr, AV_LOG_WARNING, "copy buffer %p \n", pixel_buffer);
              return pixel_buffer;
            }));

    texture_id = texture_registrar->RegisterTexture(texture_variant.get());
    if (texture_id < 0) {
        texture_registrar->UnregisterTexture(texture_id);
        return -1;
    }
    texture_ = std::move(texture_variant);
    StartRenderThread();
    return texture_id;
}

void FlutterWindowsVideoRender::Detach() {
    CHECK_VALUE(texture_id >= 0);
    StopRenderThread();
    texture_registrar->UnregisterTexture(texture_id);
    texture_id = -1;
    delete pixel_buffer->buffer;
    delete pixel_buffer;
    sws_freeContext(img_convert_ctx);
}

#endif // _FLUTTER