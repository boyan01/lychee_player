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
} VideoRenderData;


static inline VideoRenderData *get_render_data(FFP_VideoRenderContext *render_ctx) {
    return static_cast<VideoRenderData *>(render_ctx->render_callback_->opacity);
}

std::list<CPlayer *> players_;
flutter::TextureRegistrar *textures_;

void flutter_on_post_player_created(CPlayer *player) {
    players_.push_back(player);
}

void flutter_on_pre_player_free(CPlayer *player) {
    players_.remove(player);
}

void flutter_free_all_player(void (*free_handle)(CPlayer *)) {
    for (auto player : players_) {
        free_handle(player);
    }
    players_.clear();
}

static void FillColorBars(FlutterDesktopPixelBuffer *buffer, bool reverse) {
    const size_t num_bars = 8;
    const uint32_t bars[num_bars] = {0xFFFFFFFF, 0xFF00C0C0, 0xFFC0C000, 0xFF00C000, 0xFFC000C0, 0xFF0000C0, 0xFFC00000,
                                     0xFF000000};

    auto *word = (uint32_t *) buffer->buffer;

    auto width = buffer->width;
    auto height = buffer->height;
    auto column_width = width / num_bars;

    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            auto index = x / column_width;
            *(word++) = reverse ? bars[num_bars - index - 1] : bars[index];
        }
    }
}

static void render_frame(VideoRenderData *render_data, FFP_VideoRenderContext *render_ctx, Frame *frame) {
    if (!render_data->pixel_buffer->buffer) {
        std::cout << "render_frame：init" << std::endl;
        render_data->pixel_buffer->height = frame->height;
        render_data->pixel_buffer->width = frame->width;
        render_data->pixel_buffer->buffer = new uint8_t[frame->height * frame->width * 4];
    }
    auto pFrame = frame->frame;
    SwsContext *swsContext;

    swsContext = sws_getContext(pFrame->width, pFrame->height, AVPixelFormat(pFrame->format), pFrame->width,
                                pFrame->height, AV_PIX_FMT_RGBA,
                                NULL, nullptr, nullptr, nullptr);

    int linesize[4] = {frame->width * 4};
    int num_bytes = av_image_get_buffer_size(AV_PIX_FMT_ARGB, pFrame->width, pFrame->height, 1);
    std::cout << "render_frame: size = " << num_bytes << std::endl;
    uint8_t *bgr_buffer[8] = {(uint8_t *) render_data->pixel_buffer->buffer};
    sws_scale(swsContext, pFrame->data, pFrame->linesize, 0, pFrame->height, bgr_buffer, linesize);
    av_log(nullptr, AV_LOG_INFO, "render_frame: render first pixel = 0x%2x%2x%2x%2x \n",
           render_data->pixel_buffer->buffer[0],
           render_data->pixel_buffer->buffer[1], render_data->pixel_buffer->buffer[2],
           render_data->pixel_buffer->buffer[3]);
}


int64_t flutter_attach_video_render(CPlayer *player) {
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
    player->video_render_ctx.render_callback_->on_destroy = [](void *opacity) {
        delete static_cast<VideoRenderData *>(opacity);
    };
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

void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port) {
    player->message_send_port = send_port;
}

void register_flutter_plugin(flutter::PluginRegistrarWindows *registrar) {
    textures_ = registrar->texture_registrar();
    std::cout << "register_flutter_plugin: " << registrar << std::endl;
}


#endif