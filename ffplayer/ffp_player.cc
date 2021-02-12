//
// Created by yangbin on 2021/2/13.
//

#include "ffp_player.h"

CPlayer::CPlayer() {
    clock_context->Init(&audio_pkt_queue->serial,
                        &video_pkt_queue->serial,
                        [this](int av_sync_type) -> int {
                            if (data_source == nullptr) {
                                return av_sync_type;
                            }
                            if (av_sync_type == AV_SYNC_VIDEO_MASTER) {
                                if (data_source->ContainVideoStream()) {
                                    return AV_SYNC_VIDEO_MASTER;
                                } else {
                                    return AV_SYNC_AUDIO_MASTER;
                                }
                            } else if (av_sync_type == AV_SYNC_AUDIO_MASTER) {
                                if (data_source->ContainAudioStream()) {
                                    return AV_SYNC_AUDIO_MASTER;
                                } else {
                                    return AV_SYNC_EXTERNAL_CLOCK;
                                }
                            } else {
                                return AV_SYNC_EXTERNAL_CLOCK;
                            }
                        });

    message_context->Start();

    audio_render->Init(audio_pkt_queue.get(), clock_context.get());
    video_render->Init(video_pkt_queue.get(), clock_context.get(), message_context);
    decoder_context->audio_render = audio_render.get();
    decoder_context->video_render = video_render.get();
    decoder_context->clock_ctx = clock_context.get();
}

CPlayer::~CPlayer() {

}
