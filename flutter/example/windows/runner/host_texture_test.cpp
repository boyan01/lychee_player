#include "host_texture_test.h"

#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <lychee_player/lychee_host_texture.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

// Players shown one after another before the texture is unset.
constexpr int kPlayers = 4;

struct Test {
  std::string file;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;
  LycheeHostTexture* texture = nullptr;
  lychee_player* player = nullptr;
  int step = 0;
  int64_t rendered = 0;
};

Test* g_test = nullptr;

lychee_player* Play(const std::string& file) {
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = LycheeHostTextureFrameTypes();
  config.hardware_decoding = 1;
  lychee_player* player = lychee_player_create(&config);
  lychee_player_open(player, file.c_str());
  lychee_player_play(player);
  return player;
}

// Shows |next| (or nothing) and destroys the player shown before.
void Show(Test* test, lychee_player* next) {
  LycheeHostTextureSetPlayer(test->texture, next);
  if (test->player) {
    lychee_stats stats = {};
    stats.struct_size = sizeof(stats);
    lychee_player_get_stats(test->player, &stats);
    test->rendered += stats.frames_rendered;
    lychee_player_destroy(test->player);
  }
  test->player = next;
}

// One step a second, on the platform thread.
void CALLBACK Step(HWND, UINT, UINT_PTR timer, DWORD) {
  Test* test = g_test;
  const int step = ++test->step;
  if (step < kPlayers) {
    Show(test, Play(test->file));
    return;
  }
  if (step == kPlayers) {
    Show(test, nullptr);
    return;
  }
  KillTimer(nullptr, timer);
  LycheeHostTextureDestroy(test->texture);
  fprintf(stderr, "host_texture_test: ok players=%d rendered=%lld\n", kPlayers,
          static_cast<long long>(test->rendered));
  fflush(stderr);
  // The channel stays registered: it may not be destroyed in its handler.
  test->texture = nullptr;
}

}  // namespace

void StartHostTextureTest(flutter::FlutterEngine* engine) {
  char* file = nullptr;
  size_t length = 0;
  if (_dupenv_s(&file, &length, "LYCHEE_HOST_TEXTURE_TEST") != 0 || !file) {
    return;
  }
  auto* test = new Test;
  test->file = file;
  free(file);
  test->texture =
      LycheeHostTextureCreate(engine->GetRegistrarForPlugin("HostTextureTest"));
  if (!test->texture) {
    fprintf(stderr, "host_texture_test: failed to create the texture\n");
    delete test;
    return;
  }
  g_test = test;
  test->channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          engine->messenger(), "lychee_example/host_texture",
          &flutter::StandardMethodCodec::GetInstance());
  test->channel->SetMethodCallHandler([test](const auto& call, auto result) {
    if (call.method_name() != "start" || test->player || test->step > 0 ||
        !test->texture) {
      result->NotImplemented();
      return;
    }
    Show(test, Play(test->file));
    SetTimer(nullptr, 0, 1000, Step);
    result->Success(
        flutter::EncodableValue(LycheeHostTextureGetId(test->texture)));
  });
}
