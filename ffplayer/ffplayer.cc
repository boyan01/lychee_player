#include <SDL2/SDL.h>

#include "ffplayer.h"
#include "ffp_utils.h"
#include "ffp_player.h"

#if _FLUTTER

#include "ffplayer/flutter.h"
#include "third_party/dart/dart_api_dl.h"

#endif

#define CACHE_THRESHOLD_MIN_FRAMES 2

/* options specified by the user */
static AVInputFormat *file_iformat;

extern AVPacket *flush_pkt;

static inline void on_buffered_update(CPlayer *player, double position) {
    int64_t mills = position * 1000;
    player->buffered_position = mills;
    player->message_context->NotifyMsg(FFP_MSG_BUFFERING_TIME_UPDATE, mills);
}


static void change_player_state(CPlayer *player, FFPlayerState state) {
    if (player->state == state) {
        return;
    }
    player->state = state;
    player->message_context->NotifyMsg(FFP_MSG_PLAYBACK_STATE_CHANGED, state);
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

void ffplayer_seek_to_position(CPlayer *player, double position) {
    CHECK_VALUE(player);
    player->Seek(position);
}

double ffplayer_get_current_position(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, 0);
    return player->GetCurrentPosition();
}

double ffplayer_get_duration(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, -1);
    return player->GetDuration();
}

/* pause or resume the video */
void ffplayer_toggle_pause(CPlayer *player) {
    CHECK_VALUE(player);
    player->TogglePause();
}

bool ffplayer_is_mute(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, false);
    return player->IsMuted();
}

void ffplayer_set_mute(CPlayer *player, bool mute) {
    CHECK_VALUE(player);
    player->SetMute(mute);
}

void ffp_set_volume(CPlayer *player, int volume) {
    CHECK_VALUE(player);
    player->SetVolume(volume);
}

int ffp_get_volume(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, 0);
    return player->GetVolume();
}

bool ffplayer_is_paused(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, false);
    return player->IsPaused();
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
    return player->GetChapterCount();
}

int ffplayer_get_current_chapter(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, -1);
    return player->GetCurrentChapter();

}

void ffplayer_seek_to_chapter(CPlayer *player, int chapter) {
    CHECK_VALUE(player);
    player->SeekToChapter(chapter);
}

int ffplayer_open_file(CPlayer *player, const char *filename) {
    CHECK_VALUE_WITH_RETURN(player, -1);
    CHECK_VALUE_WITH_RETURN(filename, -1);
    return player->OpenDataSource(filename);
}

void ffplayer_free_player(CPlayer *player) {
#ifdef _FLUTTER
    flutter_on_pre_player_free(player);
    ffp_detach_video_render_flutter(player);
#endif
    av_log(nullptr, AV_LOG_INFO, "free play, close stream %p \n", player);
    delete player;
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
    CHECK_VALUE(player);
    player->SetMessageHandleCallback([player, callback](int32_t what, int64_t arg1, int64_t arg2) {
        callback(player, what, arg1, arg2);
    });
}

CPlayer *ffp_create_player(FFPlayerConfiguration *config) {
    auto *player = new CPlayer;
    player->TogglePause();

#ifdef _FLUTTER
    flutter_on_post_player_created(player);
#endif
    av_log(nullptr, AV_LOG_INFO, "malloc player, %p\n", player);
    player->start_configuration = *config;
    return player;
}

void ffp_refresh_texture(CPlayer *player) {
    CHECK_VALUE(player);
    player->video_render->DrawFrame();
}

void ffp_attach_video_render(CPlayer *player, FFP_VideoRenderCallback *render_callback) {
    CHECK_VALUE(player);
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
    CHECK_VALUE_WITH_RETURN(player, -1);
    return player->GetVideoAspectRatio();
}

const char *ffp_get_file_name(CPlayer *player) {
    CHECK_VALUE_WITH_RETURN(player, nullptr);
    return player->GetUrl();
}

const char *ffp_get_metadata_dict(CPlayer *player, const char *key) {
    CHECK_VALUE_WITH_RETURN(player, nullptr);
    return player->GetMetadataDict(key);
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

