//
// Created by Bin Yang on 2022/5/14.
//
#include "lychee_player_plugin.h"

#include "lychee_player.h"
#include "dart/dart_api_dl.h"

namespace {
void PostMessageToDart(Dart_Port_DL send_port, int64_t what, int64_t arg1, int64_t arg2) {
  // dart do not support int64_t array yet.
  // thanks
  https://github.com/dart-lang/sdk/issues/44384#issuecomment-738708448
  // so we pass an uint8_t array to dart isolate.
  int64_t arrays[] = {what, arg1, arg2};
  Dart_CObject dart_args = {};
  memset(&dart_args, 0, sizeof(Dart_CObject));

  dart_args.type = Dart_CObject_kTypedData;
  dart_args.value.as_typed_data.type = Dart_TypedData_kUint8;
  dart_args.value.as_typed_data.length = 3 * sizeof(int64_t);
  dart_args.value.as_typed_data.values = (uint8_t *) arrays;
  Dart_PostCObject_DL(send_port, &dart_args);
}
}

void lychee_player_initialize_dart(void *native_port) {
  Dart_InitializeApiDL(native_port);
  lychee::LycheePlayer::GlobalInit();
}

void *lychee_player_create(const char *file_path, int64_t send_port) {
  auto *player = new lychee::LycheePlayer(std::string(file_path));
  player->SetPlayerStateChangedCallback([send_port](lychee::LycheePlayer::PlayerState state) {
    PostMessageToDart(send_port, MSG_PLAYER_STATE_CHANGED, state, 0);
  });
  return player;
}

void lychee_player_dispose(void *player) {
  delete (lychee::LycheePlayer *) player;
}
void lychee_player_set_play_when_ready(void *player, bool play_when_ready) {
  if (!player) {
    return;
  }
  ((lychee::LycheePlayer *) player)->SetPlayWhenReady(play_when_ready);
}

double lychee_player_get_current_time(void *player) {
  if (!player) {
    return 0;
  }
  return ((lychee::LycheePlayer *) player)->CurrentTime();
}

double lychee_player_get_duration(void *player) {
  if (!player) {
    return 0;
  }
  return ((lychee::LycheePlayer *) player)->GetDuration();
}

void lychee_player_seek(void *player, double time) {
  if (!player) {
    return;
  }
  ((lychee::LycheePlayer *) player)->Seek(time);
}


