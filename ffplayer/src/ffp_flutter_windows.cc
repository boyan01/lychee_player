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

static flutter::TextureRegistrar *textures_;


struct VideoRenderData {
    int64_t texture_id = -1;
    FlutterDesktopPixelBuffer *pixel_buffer{nullptr};
    std::unique_ptr<flutter::TextureVariant> texture_;
    struct SwsContext *img_convert_ctx = nullptr;

public:
    ~VideoRenderData() {
        if (texture_id != -1) {
            textures_->UnregisterTexture(texture_id);
        }
        delete pixel_buffer->buffer;
        delete pixel_buffer;
        sws_freeContext(img_convert_ctx);
    }
};


void render_frame(const std::shared_ptr<VideoRenderData> &render_data, Frame *frame) {
    if (!render_data->pixel_buffer->buffer) {
        std::cout << "render_frame：init" << std::endl;
        render_data->pixel_buffer->height = frame->height;
        render_data->pixel_buffer->width = frame->width;
        render_data->pixel_buffer->buffer = new uint8_t[frame->height * frame->width * 4];
    }
    auto pFrame = frame->frame;

    render_data->img_convert_ctx = sws_getCachedContext(render_data->img_convert_ctx, pFrame->width, pFrame->height,
                                                        AVPixelFormat(pFrame->format), pFrame->width,
                                                        pFrame->height, AV_PIX_FMT_RGBA,
                                                        NULL, nullptr, nullptr, nullptr);
    if (!render_data->img_convert_ctx) {
        av_log(nullptr, AV_LOG_FATAL, "Can not initialize the conversion context\n");
        return;
    }
    int linesize[4] = {frame->width * 4};
//    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_ARGB, pFrame->width, pFrame->height, 1);
    uint8_t *bgr_buffer[8] = {(uint8_t *) render_data->pixel_buffer->buffer};
    sws_scale(render_data->img_convert_ctx, pFrame->data, pFrame->linesize, 0, pFrame->height, bgr_buffer, linesize);
#if 0
    av_log(nullptr, AV_LOG_INFO, "render_frame: render first pixel = 0x%2x%2x%2x%2x \n",
           render_data->pixel_buffer->buffer[0],
           render_data->pixel_buffer->buffer[1], render_data->pixel_buffer->buffer[2],
           render_data->pixel_buffer->buffer[3]);
#endif
}


int64_t flutter_attach_video_render(CPlayer *player) {
    if (!textures_) {
        return -1;
    }
    av_log(nullptr, AV_LOG_DEBUG, "ffp_attach_video_render_flutter \n");


//    if (player->video_render_ctx.render_callback_) {
//        auto render_data = get_render_data(&player->video_render_ctx);
//        av_log(nullptr, AV_LOG_INFO,
//               "player already attached to textures. texture_id = %lld \n", render_data->texture_id);
//        return render_data->texture_id;
//    }

    auto render_data = std::make_shared<VideoRenderData>();

    {
        auto *pixel_buffer = new FlutterDesktopPixelBuffer;
        pixel_buffer->width = 0;
        pixel_buffer->height = 0;
        pixel_buffer->buffer = nullptr;
        render_data->pixel_buffer = pixel_buffer;
    }


    auto texture_variant = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [render_data](size_t width, size_t height) -> const FlutterDesktopPixelBuffer * {
                return render_data->pixel_buffer;
            }));

    render_data->texture_id = textures_->RegisterTexture(texture_variant.get());
    render_data->texture_ = std::move(texture_variant);

    player->SetVideoRender([render_data](Frame *frame) {
        render_frame(render_data, frame);
        textures_->MarkTextureFrameAvailable(render_data->texture_id);
    });

    return render_data->texture_id;
}


void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar) {
    textures_ = registrar->texture_registrar();
    std::cout << "register_flutter_plugin: " << registrar << std::endl;
}

void flutter_detach_video_render(CPlayer *player) {
    if (!textures_) {
        av_log(nullptr, AV_LOG_FATAL, "can not detach flutter texture. textures is empty");
        return;
    }

    player->SetVideoRender(nullptr);
}


#endif // _FLUTTER