/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

#if CONFIG_AVFILTER
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#endif

#include <SDL2/SDL.h>

#include "ffplayer/ffplayer.h"
#include "ffplayer/utils.h"
#include "ffp_player_internal.h"

#if _FLUTTER

#include "ffplayer/flutter.h"
#include "third_party/dart/dart_api_dl.h"

#endif

/* options specified by the user */
static AVInputFormat *file_iformat;


#if CONFIG_AVFILTER
static int opt_add_vfilter(void *optctx, const char *opt, const char *arg) {
    GROW_ARRAY(vfilters_list, nb_vfilters);
    vfilters_list[nb_vfilters - 1] = arg;
    return 0;
}
#endif

#define CHECK_PLAYER_WITH_RETURN(PLAYER, RETURN)                               \
if(!(PLAYER)) {                                               \
    av_log(nullptr, AV_LOG_ERROR, "check player failed");                      \
    return RETURN;                                                             \
}                                                                              \

#define CHECK_PLAYER(PLAYER)                                                   \
if (!(PLAYER)) {                                              \
    av_log(nullptr, AV_LOG_ERROR, "check player failed");                      \
    return;                                                                    \
}                                                                              \

const AVRational av_time_base_q_ = {1, AV_TIME_BASE};

extern AVPacket *flush_pkt;

static inline int64_t get_valid_channel_layout(int64_t channel_layout, int channels) {
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

static inline void on_buffered_update(CPlayer *player, double position) {
    int64_t mills = position * 1000;
    player->buffered_position = mills;
    ffp_send_msg1(player, FFP_MSG_BUFFERING_TIME_UPDATE, mills);
}


static void change_player_state(CPlayer *player, FFPlayerState state) {
    if (player->state == state) {
        return;
    }
    player->state = state;
    ffp_send_msg1(player, FFP_MSG_PLAYBACK_STATE_CHANGED, state);
}

static int message_loop(void *args) {
    auto *player = static_cast<CPlayer *>(args);
    while (true) {
        Message msg = {0};
        if (player->msg_queue.Get(&msg, true) < 0) {
            break;
        }
#ifdef _FLUTTER
        if (player->message_send_port) {
            // dart do not support int64_t array yet.
            // thanks https://github.com/dart-lang/sdk/issues/44384#issuecomment-738708448
            // so we pass an uint8_t array to dart isolate.
            int64_t arrays[] = {msg.what, msg.arg1, msg.arg2};
            Dart_CObject dart_args;
            dart_args.type = Dart_CObject_kTypedData;
            dart_args.value.as_typed_data.type = Dart_TypedData_kUint8;
            dart_args.value.as_typed_data.length = 3 * sizeof(int64_t);
            dart_args.value.as_typed_data.values = (uint8_t *) arrays;
            Dart_PostCObject_DL(player->message_send_port, &dart_args);
        }
#else
        if (player->on_message) {
            player->on_message(player, msg.what, msg.arg1, msg.arg2);
        }
#endif
    }

    return 0;
}

static void on_decode_frame_block(void *opacity) {
    auto *player = static_cast<CPlayer *>(opacity);
    change_player_state(player, FFP_STATE_BUFFERING);
}

#if 0
static void stream_component_close(CPlayer *player, int stream_index) {
    VideoState *is = player->is;
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->auddec.Abort(&is->sampq);
            SDL_CloseAudioDevice(player->audio_dev);
            is->auddec.Destroy();
            swr_free(&is->swr_ctx);
            av_freep(&is->audio_buf1);
            is->audio_buf1_size = 0;
            is->audio_buf = nullptr;

            if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = nullptr;
                is->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->viddec.Abort(&is->pictq);
            is->viddec.Destroy();
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subdec.Abort(&is->subpq);
            is->subdec.Destroy();
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = nullptr;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = nullptr;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = nullptr;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}
#endif

static void stream_close(CPlayer *player) {
    VideoState *is = nullptr;
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, nullptr);

    /* close each stream */
//    if (is->audio_stream >= 0)
//        stream_component_close(player, is->audio_stream);
//    if (is->video_stream >= 0)
//        stream_component_close(player, is->video_stream);
//    if (is->subtitle_stream >= 0)
//        stream_component_close(player, is->subtitle_stream);

    change_player_state(player, FFP_STATE_IDLE);
    player->msg_queue.Abort();
    if (player->msg_tid) {
        SDL_WaitThread(player->msg_tid, nullptr);
    }

    avformat_close_input(&is->ic);

    is->videoq.Destroy();
    is->audioq.Destroy();
    is->subtitleq.Destroy();
    player->msg_queue.Abort();

    /* free all pictures */
    SDL_DestroyCond(is->continue_read_thread);
    av_free(is->filename);

    av_free(is);
    av_free(player);
}


void ffplayer_seek_to_position(CPlayer *player, double position) {
    CHECK_VALUE(player);
    CHECK_VALUE(player->data_source);
    player->data_source->Seek(position);
}

double ffplayer_get_current_position(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, 0);
    double position = player->clock_context->GetMasterClock();
    if (isnan(position)) {
        if (player->data_source) {
            position = (double) player->data_source->GetSeekPosition() / AV_TIME_BASE;
        } else {
            position = 0;
        }
    }
    return position;
}

double ffplayer_get_duration(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, -1);
    CHECK_VALUE_WITH_RETURN(player->data_source, -1);
    return player->data_source->GetDuration();
}

/* pause or resume the video */
void ffplayer_toggle_pause(CPlayer *player) {
    if (player->paused) {
        player->video_render->frame_timer +=
                av_gettime_relative() / 1000000.0 - player->clock_context->GetVideoClock()->last_updated;
        if (player->data_source->read_pause_return != AVERROR(ENOSYS)) {
            player->clock_context->GetVideoClock()->paused = 0;
        }
        player->clock_context->GetVideoClock()->SetClock(player->clock_context->GetVideoClock()->GetClock(),
                                                         player->clock_context->GetVideoClock()->serial);
    }
    player->clock_context->GetExtClock()->SetClock(player->clock_context->GetExtClock()->GetClock(),
                                                   player->clock_context->GetExtClock()->serial);

    player->paused = !player->paused;
    player->clock_context->GetExtClock()->paused = player->clock_context->GetAudioClock()->paused
            = player->clock_context->GetVideoClock()->paused = player->paused;
    if (player->data_source) {
        player->data_source->paused = player->paused;
    }
    player->audio_render->paused = player->paused;
    player->video_render->paused_ = player->paused;
    player->video_render->step = false;
}

bool ffplayer_is_mute(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, false);
    return player->audio_render->IsMute();
}

void ffplayer_set_mute(CPlayer *player, bool mute) {
    CHECK_VALUE(player);
    player->audio_render->SetMute(mute);
}

void ffp_set_volume(CPlayer *player, int volume) {
    CHECK_VALUE(player);
    player->audio_render->SetVolume(volume);
}

int ffp_get_volume(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, 0);
    return player->audio_render->GetVolume();
}

bool ffplayer_is_paused(CPlayer *player) {
    CHECK_PLAYER_WITH_RETURN(player, false);
    return player->paused;
}

#define BUFFERING_CHECK_PER_MILLISECONDS                     (500)
#define BUFFERING_CHECK_PER_MILLISECONDS_NO_RENDERING        (20)

#if 0
static void check_buffering(CPlayer *player) {
    VideoState *is = player->is;
    if (is->eof) {
        double position = ffplayer_get_duration(player);
        if (position <= 0 && is->audioq.last_pkt) {
            position = av_q2d(is->audio_st->time_base) *
                       (int64_t) (is->audioq.last_pkt->pkt.pts + is->audioq.last_pkt->pkt.duration);
        }
        if (position <= 0 && is->videoq.last_pkt) {
            position = av_q2d(is->video_st->time_base) *
                       (int64_t) (is->videoq.last_pkt->pkt.pts + is->videoq.last_pkt->pkt.duration);
        }
        on_buffered_update(player, position);
        change_player_state(player, FFP_STATE_READY);
        return;
    }

    int64_t current_ts = av_gettime_relative() / 1000;
    int step = player->state == FFP_STATE_BUFFERING ? BUFFERING_CHECK_PER_MILLISECONDS_NO_RENDERING
                                                    : BUFFERING_CHECK_PER_MILLISECONDS;
    if (current_ts - player->last_io_buffering_ts < step) {
        return;
    }
    if (player->state == FFP_STATE_END || player->state == FFP_STATE_IDLE) {
        return;
    }
    player->last_io_buffering_ts = current_ts;
    double cached_position = INT_MAX;
    int nb_packets = INT_MAX;
    if (is->audio_st && is->audioq.last_pkt) {
        nb_packets = FFP_MIN(nb_packets, player->is->audioq.nb_packets);
        double audio_position = player->is->audioq.last_pkt->pkt.pts * av_q2d(player->is->audio_st->time_base);
        cached_position = FFMIN(cached_position, audio_position);
        av_log(nullptr, AV_LOG_DEBUG, "audio q cached: %f \n",
               player->is->audioq.duration * av_q2d(player->is->audio_st->time_base));
    }
    if (is->video_st && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        cached_position = FFP_MIN(cached_position,
                                  player->is->videoq.duration * av_q2d(player->is->video_st->time_base));
        nb_packets = FFP_MIN(nb_packets, player->is->videoq.nb_packets);
        av_log(nullptr, AV_LOG_DEBUG, "video q cached: %f \n",
               player->is->videoq.duration * av_q2d(player->is->video_st->time_base));
    }
    if (is->subtitle_st) {
        cached_position = FFP_MIN(cached_position,
                                  player->is->subtitleq.duration * av_q2d(player->is->subtitle_st->time_base));
        nb_packets = FFP_MIN(nb_packets, player->is->subtitleq.nb_packets);
        av_log(nullptr, AV_LOG_DEBUG, "subtitle q cached: %f \n",
               player->is->subtitleq.duration * av_q2d(player->is->audio_st->time_base));
    }
    if (cached_position == INT_MAX) {
        av_log(nullptr, AV_LOG_ERROR, "check_buffering failed\n");
        return;
    }
    on_buffered_update(player, cached_position);

    bool ready = false;
    if (is->video_st) {

    }
    if ((player->is->videoq.nb_packets > CACHE_THRESHOLD_MIN_FRAMES || player->is->video_stream < 0 ||
         player->is->videoq.abort_request)
        && (player->is->audioq.nb_packets > CACHE_THRESHOLD_MIN_FRAMES || player->is->audio_stream < 0 ||
            player->is->audioq.abort_request)
        && (player->is->subtitleq.nb_packets > CACHE_THRESHOLD_MIN_FRAMES || player->is->subtitle_stream < 0 ||
            player->is->subtitleq.abort_request)) {
        change_player_state(player, FFP_STATE_READY);
    }

}
#endif


int ffplayer_get_chapter_count(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, -1);
    CHECK_VALUE_WITH_RETURN(player->data_source, -1);
    return player->data_source->GetChapterCount();
}

int ffplayer_get_current_chapter(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, -1);
    CHECK_VALUE_WITH_RETURN(player->data_source, -1);
    int64_t pos = ffplayer_get_current_position(player) * AV_TIME_BASE;
    return player->data_source->GetChapterByPosition(pos);
}

void ffplayer_seek_to_chapter(CPlayer *player, int chapter) {
    CHECK_VALUE(player);
    CHECK_VALUE(player->data_source);
    player->data_source->SeekToChapter(chapter);
}

static CPlayer *ffplayer_alloc_player() {
    auto *player = new CPlayer;
    player->audio_disable = 0;
    player->video_disable = 0;
    player->subtitle_disable = 0;

    player->seek_by_bytes = -1;

    player->show_status = -1;
    player->start_time = AV_NOPTS_VALUE;
    player->duration = AV_NOPTS_VALUE;
    player->fast = 0;
    player->genpts = 0;
    player->lowres = 0;
    player->decoder_reorder_pts = -1;

    player->loop = 1;
    player->framedrop = -1;
    player->infinite_buffer = -1;
    player->show_mode = SHOW_MODE_NONE;

    player->audio_codec_name = nullptr;
    player->subtitle_codec_name = nullptr;
    player->video_codec_name = nullptr;

    player->rdftspeed = 0.02;

    player->autorotate = 1;
    player->find_stream_info = 1;
    player->filter_nbthreads = 0;

    player->audio_callback_time = 0;

    player->audio_dev = 0;

    player->on_load_metadata = nullptr;
    player->on_message = nullptr;

    player->buffered_position = -1;
    player->state = FFP_STATE_IDLE;
    player->last_io_buffering_ts = -1;

    player->msg_queue.Init();

    ffplayer_toggle_pause(player);

#ifdef _FLUTTER
    flutter_on_post_player_created(player);
#endif
    av_log(nullptr, AV_LOG_INFO, "malloc player, %p\n", player);
    return player;
}

int ffplayer_open_file(CPlayer *player, const char *filename) {
//    stream_open(player, filename, file_iformat);
    if (player->data_source) {
        av_log(nullptr, AV_LOG_ERROR, "can not open file multi-times.\n");
        return -1;
    }

    player->data_source = new DataSource(filename, file_iformat);
    player->data_source->audio_queue = player->audio_pkt_queue.get();
    player->data_source->video_queue = player->video_pkt_queue.get();
    player->data_source->subtitle_queue = player->subtitle_pkt_queue.get();
    player->data_source->ext_clock = player->clock_context->GetAudioClock();
    player->data_source->decoder_ctx = player->decoder_context.get();

    player->data_source->Open(player);

    return 0;
}

void ffplayer_free_player(CPlayer *player) {
#ifdef _FLUTTER
    flutter_on_pre_player_free(player);
    ffp_detach_video_render_flutter(player);
#endif
    av_log(nullptr, AV_LOG_INFO, "free play, close stream %p \n", player);
    stream_close(player);
}

void ffplayer_global_init(void *arg) {
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    av_log_set_level(AV_LOG_INFO);
    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
        av_log(nullptr, AV_LOG_ERROR, "SDL fails to initialize audio subsystem!\n%s", SDL_GetError());
    else
        av_log(nullptr, AV_LOG_DEBUG, "SDL Audio was initialized fine!\n");

    flush_pkt = new AVPacket;
    av_init_packet(flush_pkt);
    flush_pkt->data = (uint8_t *) &flush_pkt;

#ifdef _FLUTTER
    assert(arg);
    Dart_InitializeApiDL(arg);
    flutter_free_all_player([](CPlayer *player) {
        av_log(nullptr, AV_LOG_INFO, "free play, close stream %p by flutter global \n", player);
        stream_close(player);
    });
#endif
}


void ffp_set_message_callback(CPlayer *player, void (*callback)(CPlayer *, int32_t, int64_t, int64_t)) {
    CHECK_PLAYER(player);
    player->message_context->message_callback = [player, callback](int32_t what, int64_t arg1, int64_t arg2) {
        callback(player, what, arg1, arg2);
    };
}

CPlayer *ffp_create_player(FFPlayerConfiguration *config) {
    CPlayer *player = ffplayer_alloc_player();
    if (!player) {
        return nullptr;
    }
    player->audio_disable = config->audio_disable;
    player->video_disable = config->video_disable;
    player->subtitle_disable = config->subtitle_disable;
    player->seek_by_bytes = config->seek_by_bytes;
    player->show_status = config->show_status;
    player->start_time = config->start_time;
    player->loop = config->loop;

    return player;
}

void ffp_refresh_texture(CPlayer *player, void(*on_locked)(VideoRender *video_render_ctx)) {
    CHECK_PLAYER(player);
    player->video_render->DrawFrame();
}

void ffp_attach_video_render(CPlayer *player, FFP_VideoRenderCallback *render_callback) {
    CHECK_PLAYER(player);
    if (player->video_render->render_attached) {
        av_log(nullptr, AV_LOG_FATAL, "video_render_already attached.\n");
        return;
    }
    if (render_callback == nullptr) {
        av_log(nullptr, AV_LOG_ERROR, "can not attach null render_callback.\n");
        return;
    }
    player->video_render->render_callback_ = render_callback;
    if (player->video_render->Start()) {
        player->video_render->render_attached = true;
    }
}

double ffp_get_video_aspect_ratio(CPlayer *player) {
    CHECK_PLAYER_WITH_RETURN(player, -1);
    return player->video_render->GetVideoAspectRatio();
}

const char *ffp_get_file_name(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, nullptr);
    CHECK_VALUE_WITH_RETURN(player->data_source, nullptr);
    return player->data_source->GetFileName();
}

const char *ffp_get_metadata_dict(CPlayer *player, const char *key) {
    CHECK_VALUE_WITH_RETURN(player, nullptr);
    CHECK_VALUE_WITH_RETURN(player->data_source, nullptr);
    return player->data_source->GetMetadataDict(key);
}


#ifdef _FLUTTER

int64_t ffp_attach_video_render_flutter(CPlayer *player) {
    int64_t texture_id = flutter_attach_video_render(player);
    auto render_ctx = &player->video_render_ctx;
    if (render_ctx->render_callback_ && !render_ctx->render_thread_) {
        start_video_render(player);
    }
    return texture_id;
}

void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port) {
    player->message_send_port = send_port;
}

void ffp_detach_video_render_flutter(CPlayer *player) {
    CHECK_PLAYER(player);
    flutter_detach_video_render(player);
}
#endif // _FLUTTER

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
    memset(wanted_stream_spec, 0, sizeof wanted_stream_spec);

    message_context->Start();

    audio_render->Init(audio_pkt_queue.get(), clock_context.get());
    video_render->Init(video_pkt_queue.get(), clock_context.get(), message_context);
    decoder_context->audio_render = audio_render.get();
    decoder_context->video_render = video_render.get();
    decoder_context->clock_ctx = clock_context.get();
}

CPlayer::~CPlayer() {

}
