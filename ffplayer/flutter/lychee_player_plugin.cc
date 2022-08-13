#include <algorithm>
#include <iostream>
#include <list>

#include "dart/dart_api_dl.h"
#include "lychee_player_plugin.h"

#include "media_player.h"

#ifdef LYCHEE_ENABLE_SDL
#include "audio_render_sdl.h"
#elif defined(LYCHEE_OSX)
#include "audio_render_core_audio.h"
#endif

namespace {

// hold all player instance. destroy all play when flutter app hot reloaded.
std::list<MediaPlayer*>* players_;

void ReleasePlayer(MediaPlayer* player) {
  delete player;
}

void PostMessageToDart(Dart_Port_DL send_port,
                       int64_t what,
                       int64_t arg1,
                       int64_t arg2) {
  // dart do not support int64_t array yet.
  // https://github.com/dart-lang/sdk/issues/44384#issuecomment-738708448
  // so we pass an uint8_t array to dart isolate.
  int64_t arrays[] = {what, arg1, arg2};
  Dart_CObject dart_args = {};
  memset(&dart_args, 0, sizeof(Dart_CObject));

  dart_args.type = Dart_CObject_kTypedData;
  dart_args.value.as_typed_data.type = Dart_TypedData_kUint8;
  dart_args.value.as_typed_data.length = 3 * sizeof(int64_t);
  dart_args.value.as_typed_data.values = (uint8_t*)arrays;
  Dart_PostCObject_DL(send_port, &dart_args);
}

}  // namespace

void lychee_player_initialize_dart(void* native_port) {
  Dart_InitializeApiDL(native_port);
  MediaPlayer::GlobalInit();

  if (players_) {
    for (const auto& player : *players_) {
      av_log(nullptr, AV_LOG_INFO,
             "free play, close stream %p by flutter global \n", player);
      ReleasePlayer(player);
    }
    players_->clear();
  }
}

void* lychee_player_create(const char* file_path, int64_t send_port) {
  std::unique_ptr<VideoRenderBase> video_render = nullptr;
  std::unique_ptr<BasicAudioRender> audio_render;
#ifdef LYCHEE_ENABLE_SDL
  audio_render = std::make_unique<AudioRenderSdl>();
#elif _FLUTTER_MEDIA_ANDROID
  video_render = std::make_unique<media::FlutterAndroidVideoRender>();
  audio_render = std::make_unique<media::AudioRenderOboe>();
#elif defined(LYCHEE_OSX)
  audio_render = std::make_unique<AudioRenderCoreAudio>();
#else
  video_render = nullptr;
  audio_render = std::make_unique<AudioRenderSdl>();
#endif
  auto* player =
      new MediaPlayer(std::move(video_render), std::move(audio_render));
  player->OpenDataSource(file_path);
  player->SetMessageHandleCallback(
      [send_port](int what, int64_t arg1, int64_t arg2) {
        PostMessageToDart(send_port, what, arg1, arg2);
      });
  if (!players_) {
    players_ = new std::list<MediaPlayer*>;
  }
  players_->push_back(player);
  return player;
}

void lychee_player_dispose(void* player) {
  if (!player) {
    return;
  }
  auto* p = static_cast<MediaPlayer*>(player);
  if (players_) {
    players_->remove(p);
  }
  ReleasePlayer(p);
}

void lychee_player_set_play_when_ready(void* player, bool play_when_ready) {
  if (!player) {
    return;
  }
  auto* p = static_cast<MediaPlayer*>(player);
  p->SetPlayWhenReady(play_when_ready);
}

void lychee_player_seek(void* player, double time) {
  if (!player) {
    return;
  }
  auto* p = static_cast<MediaPlayer*>(player);
  p->Seek(time);
}

double lychee_player_get_current_time(void* player) {
  if (!player) {
    return 0;
  }
  auto* p = static_cast<MediaPlayer*>(player);
  return p->GetCurrentPosition();
}

double lychee_player_get_duration(void* player) {
  if (!player) {
    return 0;
  }
  auto* p = static_cast<MediaPlayer*>(player);
  return p->GetDuration();
}

void lychee_player_set_volume(void* player, int volume) {
  if (!player) {
    return;
  }
  std::cout << "set volume " << volume << std::endl;
  auto* p = static_cast<MediaPlayer*>(player);
  p->SetVolume(volume);
}

int lychee_player_get_volume(void* player) {
  if (!player) {
    return 0;
  }
  auto* p = static_cast<MediaPlayer*>(player);
  return p->GetVolume();
}
