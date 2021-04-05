#include <list>

#include "ffplayer.h"
#include "media_player.h"

#include "ffp_flutter.h"
#include "dart/dart_api_dl.h"

#if defined(_FLUTTER_MEDIA_WINDOWS)
#include "ffp_flutter_windows.h"
#elif defined(_FLUTTER_MEDIA_ANDROID)
#include "ffp_flutter_android.h"
#elif defined(_FLUTTER_MEDIA_LINUX)
#define _MEDIA_AUDIO_USE_SDL
#elif defined(_FLUTTER_MEDIA_MACOS)
#define _MEDIA_AUDIO_USE_SDL
#endif

// Use SDL2 to render audio.
#ifdef _MEDIA_AUDIO_USE_SDL
#include "audio_render_sdl.h"
#include "sdl_utils.h"
#endif

// hold all player instance. destroy all play when flutter app hot reloaded.
static std::list<CPlayer *> *players_;

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

void media_set_play_when_ready(MediaPlayer *player, bool play_when_ready) {
  CHECK_VALUE(player);
  player->SetPlayWhenReady(play_when_ready);
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
  return player->IsPlayWhenReady();
}

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

static void release_player(CPlayer *player) {
  ffp_detach_video_render_flutter(player);
  av_log(nullptr, AV_LOG_INFO, "free play, close stream %p \n", player);
  delete player;
}

void ffplayer_free_player(CPlayer *player) {
  if (players_) {
    players_->remove(player);
  }
  release_player(player);
}

void ffplayer_global_init(void *arg) {
  assert(arg);
#if __ANDROID__
  av_log_set_callback([](void *ptr, int level, const char *format, va_list v_args) {
    int prio;
    switch (level) {
      case AV_LOG_VERBOSE:prio = ANDROID_LOG_VERBOSE;
        break;
      case AV_LOG_DEBUG:prio = ANDROID_LOG_DEBUG;
        break;
      case AV_LOG_INFO:prio = ANDROID_LOG_INFO;
        break;
      case AV_LOG_WARNING:prio = ANDROID_LOG_WARN;
        break;
      case AV_LOG_ERROR:prio = ANDROID_LOG_ERROR;
        break;
      case AV_LOG_FATAL:prio = ANDROID_LOG_FATAL;
        break;
      default:prio = ANDROID_LOG_UNKNOWN;
        break;
    }

    va_list vl2;
    char *line = static_cast<char *>(malloc(128 * sizeof(char)));
    static int print_prefix = 1;
    va_copy(vl2, v_args);
    av_log_format_line(ptr, level, format, vl2, line, 128, &print_prefix);
    va_end(vl2);
    line[127] = '\0';
    __android_log_print(prio, "FF_PLAYER", "%s", line);
    free(line);
  });
#endif
  CPlayer::GlobalInit();

#ifdef _MEDIA_AUDIO_USE_SDL
  media::sdl::InitSdlAudio();
#endif

  Dart_InitializeApiDL(arg);

  if (players_) {
    for (const auto &player : *players_) {
      av_log(nullptr, AV_LOG_INFO, "free play, close stream %p by flutter global \n", player);
      release_player(player);
    }
    players_->clear();
  }
}

CPlayer *ffp_create_player(PlayerConfiguration *config) {
  std::unique_ptr<VideoRenderBase> video_render;
  std::unique_ptr<BasicAudioRender> audio_render;
#ifdef _FLUTTER_MEDIA_WINDOWS
  video_render = std::make_unique<FlutterWindowsVideoRender>();
  audio_render = std::make_unique<AudioRenderSdl>();
#elif _FLUTTER_MEDIA_ANDROID
  video_render = std::make_unique<media::FlutterAndroidVideoRender>();
  audio_render = std::make_unique<media::AudioRenderOboe>();
#else
  video_render = nullptr;
  audio_render = std::make_unique<AudioRenderSdl>();
#endif
  auto *player = new MediaPlayer(std::move(video_render), std::move(audio_render));
  player->SetPlayWhenReady(true);
  av_log(nullptr, AV_LOG_INFO, "malloc player, %p\n", player);
  player->start_configuration = *config;

  if (!players_) {
    players_ = new std::list<CPlayer *>;
  }
  players_->push_back(player);

  return player;
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
  int64_t texture_id = -1;
#ifdef _FLUTTER_MEDIA_WINDOWS
  auto *video_render = dynamic_cast<FlutterWindowsVideoRender *>(player->GetVideoRender());
  texture_id = video_render->Attach();
#elif _FLUTTER_MEDIA_ANDROID
  auto *video_render = dynamic_cast<media::FlutterAndroidVideoRender *>(player->GetVideoRender());
  texture_id = video_render->Attach();
#elif _FLUTTER_MEDIA_LINUX

#endif
  av_log(nullptr, AV_LOG_INFO, "ffp_attach_video_render_flutter: id = %lld\n", texture_id);
  return texture_id;
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
#ifdef _FLUTTER_MEDIA_WINDOWS
  auto *video_render = dynamic_cast<FlutterWindowsVideoRender *>(player->GetVideoRender());
  video_render->Detach();
#elif _FLUTTER_MEDIA_ANDROID
  auto *video_render = dynamic_cast<media::FlutterAndroidVideoRender *>(player->GetVideoRender());
  video_render->Detach();
#endif
}