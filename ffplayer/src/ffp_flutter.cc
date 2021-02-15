
#ifdef _FLUTTER

#include <list>

#include "ffplayer.h"
#include "ffp_utils.h"
#include "ffp_player.h"

#include "ffp_flutter.h"
#include "dart/dart_api_dl.h"

#if defined(_FLUTTER_WINDOWS)
#include "ffp_flutter_windows.h"
#endif

// hold all player instance. destroy all play when flutter app hot reloaded.
static std::list<CPlayer *> *players_;

static inline void on_buffered_update(CPlayer *player, double position) {
  int64_t mills = position * 1000;
  player->buffered_position = mills;
//    player->message_context->NotifyMsg(FFP_MSG_BUFFERING_TIME_UPDATE, mills);
}

static void change_player_state(CPlayer *player, FFPlayerState state) {
  if (player->state == state) {
    return;
  }
  player->state = state;
//    player->message_context->NotifyMsg(FFP_MSG_PLAYBACK_STATE_CHANGED, state);
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
  if (players_) {
    players_->remove(player);
  }
  ffp_detach_video_render_flutter(player);
  av_log(nullptr, AV_LOG_INFO, "free play, close stream %p \n", player);
  delete player;
}

void ffplayer_global_init(void *arg) {
  assert(arg);
  CPlayer::GlobalInit();
  Dart_InitializeApiDL(arg);

  if (players_) {
    for (auto player : *players_) {
      av_log(nullptr, AV_LOG_INFO, "free play, close stream %p by flutter global \n", player);
      ffplayer_free_player(player);
    }
    players_->clear();
  }
}

CPlayer *ffp_create_player(PlayerConfiguration *config) {
  auto *player = new CPlayer;
  player->TogglePause();
  av_log(nullptr, AV_LOG_INFO, "malloc player, %p\n", player);
  player->start_configuration = *config;

  if (!players_) {
    players_ = new std::list<CPlayer *>;
  }
  players_->push_back(player);

  return player;
}

void ffp_refresh_texture(CPlayer *player) {
  CHECK_VALUE(player);
  player->DrawFrame();
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

int64_t ffp_attach_video_render_flutter(CPlayer *player) {
#ifdef _FLUTTER_WINDOWS
  return flutter_attach_video_render(player);
#elif _FLUTTER_LINUX
  return -1;
#endif
}

void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port) {
  player->SetMessageHandleCallback([send_port](int32_t what, int64_t arg1, int64_t arg2) {
    // dart do not support int64_t array yet.
    // thanks https://github.com/dart-lang/sdk/issues/44384#issuecomment-738708448
    // so we pass an uint8_t array to dart isolate.
    int64_t arrays[] = {what, arg1, arg2};
    Dart_CObject dart_args = {};
    memset(&dart_args, 0, sizeof(Dart_CObject));

    dart_args.type = Dart_CObject_kTypedData;
    dart_args.value.as_typed_data.type = Dart_TypedData_kUint8;
    dart_args.value.as_typed_data.length = 3 * sizeof(int64_t);
    dart_args.value.as_typed_data.values = (uint8_t *) arrays;
    Dart_PostCObject_DL(send_port, &dart_args);
  });
}

void ffp_detach_video_render_flutter(CPlayer *player) {
  CHECK_VALUE(player);
#ifdef _FLUTTER_WINDOWS
  flutter_detach_video_render(player);
#endif
}

#endif // _FLUTTER