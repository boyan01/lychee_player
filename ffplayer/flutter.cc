//
// Created by boyan on 2021/1/24.
//

#ifdef _FLUTTER

#include <list>
#include <iostream>
#include <cstdlib>

#include "ffplayer/flutter.h"
#include "flutter_texture_registrar.h"


typedef struct VideoRenderData_ {
    int64_t texture_id{0};
    FlutterDesktopPixelBuffer *pixel_buffer{nullptr};
    std::unique_ptr<flutter::TextureVariant> texture_;
    std::mutex *mutex{nullptr};
    struct SwsContext *img_convert_ctx = nullptr;
} VideoRenderData;


static inline VideoRenderData *get_render_data(FFP_VideoRenderContext *render_ctx) {
    return static_cast<VideoRenderData *>(render_ctx->render_callback_->opacity);
}

static std::list<CPlayer *> *players_;
static flutter::TextureRegistrar *textures_;

void flutter_on_post_player_created(CPlayer *player) {
    if (!players_) {
        players_ = new std::list<CPlayer *>;
    }
    players_->push_back(player);
}

void flutter_on_pre_player_free(CPlayer *player) {
    if (!players_) {
        return;
    }
    players_->remove(player);
}

void flutter_free_all_player(void (*free_handle)(CPlayer *)) {
    if (!players_) {
        return;
    }
    for (auto player : *players_) {
        free_handle(player);
    }
    players_->clear();
}


void render_frame(VideoRenderData *render_data, FFP_VideoRenderContext *render_ctx, Frame *frame) {
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
    std::cout << "ffp_attach_video_render_flutter" << std::endl;
    auto render_data = new VideoRenderData;
    render_data->pixel_buffer = new FlutterDesktopPixelBuffer;
    render_data->pixel_buffer->width = 0;
    render_data->pixel_buffer->height = 0;
    render_data->pixel_buffer->buffer = nullptr;
    player->video_render_ctx.render_callback_ = new FFP_VideoRenderCallback;
    auto texture_ = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [render_data](size_t width, size_t height) -> const FlutterDesktopPixelBuffer * {
                return render_data->pixel_buffer;
            }));

    render_data->texture_id = textures_->RegisterTexture(texture_.get());
    render_data->texture_ = std::move(texture_);
    render_data->mutex = new std::mutex();
    player->video_render_ctx.render_callback_->opacity = render_data;
//    player->video_render_ctx.render_callback_->on_destroy = [](void *opacity) {
//        auto render_data = static_cast<VideoRenderData *>(opacity);
//        textures_->UnregisterTexture(render_data->texture_id);
//        delete render_data->mutex;
//        delete render_data->pixel_buffer;
//        delete render_data;
//    };
    player->video_render_ctx.render_callback_->on_render = [](FFP_VideoRenderContext *video_render_ctx, Frame *frame) {
        auto render_data = get_render_data(video_render_ctx);
        render_frame(render_data, video_render_ctx, frame);
    };
    player->video_render_ctx.render_callback_->on_texture_updated = [](FFP_VideoRenderContext *video_render_ctx) {
        auto render_data = get_render_data(video_render_ctx);
        textures_->MarkTextureFrameAvailable(render_data->texture_id);
    };
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
    auto *render_data = get_render_data(&player->video_render_ctx);
    textures_->UnregisterTexture(render_data->texture_id);

    player->video_render_ctx.render_mutex_->lock();
    auto render_callback = player->video_render_ctx.render_callback_;
    player->video_render_ctx.render_callback_ = nullptr;
    render_callback->opacity = nullptr;
    render_callback->on_render = nullptr;
    render_callback->on_destroy = nullptr;
    delete render_callback;
    player->video_render_ctx.render_mutex_->unlock();

    delete render_data->pixel_buffer;
    render_data->texture_ = nullptr;
    sws_freeContext(render_data->img_convert_ctx);
    delete render_data->mutex;
    delete render_data;
}


#endif // _FLUTTER