#include <list>

#if defined(_MEDIA_MACOS) || defined(_MEDIA_IOS)
#define _MEDIA_DARWIN 1
#endif

#if defined(_MEDIA_WINDOWS)
#define _MEDIA_AUDIO_USE_SDL
#elif defined(_MEDIA_ANDROID)
#include "android/log.h"
#include "oboe_audio_renderer_sink.h"
#elif defined(_MEDIA_LINUX)
#define _MEDIA_AUDIO_USE_SDL
#elif defined(_MEDIA_DARWIN)
#include "macos_audio_renderer_sink.h"
#endif

#include "ffplayer.h"
#include "media_player.h"
#include "external_video_renderer_sink.h"

#include "ffp_flutter.h"
#include "dart/dart_api_dl.h"

// Use SDL2 to render audio.
#ifdef _MEDIA_AUDIO_USE_SDL
#include "sdl_audio_renderer_sink.h"
#include "sdl_utils.h"
#endif

#define CHECK_VALUE_WITH_RETURN(VALUE, RETURN)\
if(!(VALUE)) {\
    av_log(nullptr, AV_LOG_ERROR, "check %s value failed in %s\n", #VALUE, __FUNCTION__);\
    return RETURN;\
}\

#define CHECK_VALUE(VALUE)\
if(!(VALUE)) {\
    av_log(nullptr, AV_LOG_ERROR, "check %s value failed in %s\n", #VALUE, __FUNCTION__);\
    return;\
}                         \

// hold all player instance. destroy all play when flutter app hot reloaded.
static std::list<std::shared_ptr<CPlayer>> players_;

void ffplayer_seek_to_position(CPlayer *player, double position) {
  CHECK_VALUE(player);
  player->Seek(TimeDelta::FromSecondsD(position));
}

double ffplayer_get_current_position(CPlayer *player) {
  CHECK_VALUE_WITH_RETURN(player, 0);
  return player->GetCurrentPosition().InSecondsF();
}

double ffplayer_get_duration(CPlayer *player) {
  CHECK_VALUE_WITH_RETURN(player, -1);
  return player->GetDuration();
}

void media_set_play_when_ready(MediaPlayer *player, bool play_when_ready) {
  CHECK_VALUE(player);
  player->SetPlayWhenReady(play_when_ready);
}

void ffp_set_volume(CPlayer *player, double volume) {
  CHECK_VALUE(player);
  player->SetVolume(volume);
}

double ffp_get_volume(CPlayer *player) {
  CHECK_VALUE_WITH_RETURN(player, 0);
  return player->GetVolume();
}

bool ffplayer_is_paused(CPlayer *player) {
  CHECK_VALUE_WITH_RETURN(player, false);
  return player->IsPlayWhenReady();
}

int ffplayer_open_file(CPlayer *player, const char *filename) {
  CHECK_VALUE_WITH_RETURN(player, -1);
  CHECK_VALUE_WITH_RETURN(filename, -1);
  return player->OpenDataSource(filename);
}

static void release_player(CPlayer *player) {
  ffp_detach_video_render_flutter(player);
  av_log(nullptr, AV_LOG_INFO, "free play, close stream %p \n", player);
}

void ffplayer_free_player(CPlayer *player) {
  release_player(player);
  players_.remove_if([player](const std::shared_ptr<MediaPlayer> &ptr) {
    return ptr.get() == player;
  });
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

  for (const auto &player: players_) {
    av_log(nullptr, AV_LOG_INFO, "free play, close stream %p by flutter global \n", player.get());
    release_player(player.get());
  }
  players_.clear();
}

CPlayer *ffp_create_player(PlayerConfiguration *config) {
  std::unique_ptr<VideoRendererSink> video_render;
  std::unique_ptr<AudioRendererSink> audio_render;
#ifdef _MEDIA_WINDOWS
  video_render = std::make_unique<ExternalVideoRendererSink>();
  audio_render = std::make_unique<SdlAudioRendererSink>();
#elif _MEDIA_ANDROID
  video_render = std::make_unique<media::ExternalVideoRendererSink>();
  audio_render = std::make_unique<media::OboeAudioRendererSink>();
#elif _MEDIA_DARWIN
  video_render = std::make_unique<media::ExternalVideoRendererSink>();
  audio_render = std::make_unique<media::MacosAudioRendererSink>();
#elif _MEDIA_LINUX
  video_render = std::make_unique<media::NullVideoRendererSink>();
  audio_render = std::make_unique<media::SdlAudioRendererSink>();
#endif
  auto player = std::make_shared<MediaPlayer>(
      std::move(video_render), std::move(audio_render),
      TaskRunner(MessageLooper::PrepareLooper("media_player")));
  player->SetPlayWhenReady(true);
  av_log(nullptr, AV_LOG_INFO, "malloc player, %p\n", player.get());
  player->start_configuration = *config;

  players_.push_back(player);

  return player.get();
}

double ffp_get_video_aspect_ratio(CPlayer *player) {
  CHECK_VALUE_WITH_RETURN(player, -1);
  // TODO FIXME
  return 16.0 / 9;
}

// TODO rename this
int64_t ffp_attach_video_render_flutter(CPlayer *player) {
  auto *video_render_sink = dynamic_cast<media::ExternalVideoRendererSink *>(player->GetVideoRenderSink());
  auto texture_id = video_render_sink->texture_id();
  DLOG(INFO) << "ffp_attach_video_render_flutter: id = " << texture_id;
  return texture_id;
}

void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port) {
//  player->SetMessageHandleCallback([send_port](int32_t what, int64_t arg1, int64_t arg2) {
//    // dart do not support int64_t array yet.
//    // thanks https://github.com/dart-lang/sdk/issues/44384#issuecomment-738708448
//    // so we pass an uint8_t array to dart isolate.
//    int64_t arrays[] = {what, arg1, arg2};
//    Dart_CObject dart_args = {};
//    memset(&dart_args, 0, sizeof(Dart_CObject));
//
//    dart_args.type = Dart_CObject_kTypedData;
//    dart_args.value.as_typed_data.type = Dart_TypedData_kUint8;
//    dart_args.value.as_typed_data.length = 3 * sizeof(int64_t);
//    dart_args.value.as_typed_data.values = (uint8_t *) arrays;
//    Dart_PostCObject_DL(send_port, &dart_args);
//  });
}

// TODO remove this method.
void ffp_detach_video_render_flutter(CPlayer *player) {
  //  DO NOTHING. since we do not support remove textures dynamic.
}

void windows_register_texture(void *callback) {
  register_external_texture_factory((FlutterTextureAdapterFactory) callback);
}
