#include "host_texture_test.h"

#include <lychee_player/lychee_host_texture.h>

#include <cstdio>
#include <cstdlib>

namespace {

// Players shown one after another before the texture is unset.
constexpr int kPlayers = 4;

struct Test {
  const char* file;
  FlMethodChannel* channel;
  LycheeHostTexture* texture;
  lychee_player* player;
  int step;
  int64_t rendered;
};

lychee_player* play(const char* file) {
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = lychee_host_texture_frame_types();
  config.hardware_decoding = 1;
  lychee_player* player = lychee_player_create(&config);
  lychee_player_open(player, file);
  lychee_player_play(player);
  return player;
}

// Shows |next| (or nothing) and destroys the player shown before.
void show(Test* test, lychee_player* next) {
  lychee_host_texture_set_player(test->texture, next);
  if (test->player) {
    lychee_stats stats = {};
    stats.struct_size = sizeof(stats);
    lychee_player_get_stats(test->player, &stats);
    test->rendered += stats.frames_rendered;
    lychee_player_destroy(test->player);
  }
  test->player = next;
}

// One step a second, on the main thread.
gboolean step(gpointer data) {
  Test* test = static_cast<Test*>(data);
  const int step = ++test->step;
  if (step < kPlayers) {
    show(test, play(test->file));
    return G_SOURCE_CONTINUE;
  }
  if (step == kPlayers) {
    show(test, nullptr);
    return G_SOURCE_CONTINUE;
  }
  lychee_host_texture_free(test->texture);
  fprintf(stderr, "host_texture_test: ok players=%d rendered=%ld\n", kPlayers,
          static_cast<long>(test->rendered));
  g_object_unref(test->channel);
  delete test;
  return G_SOURCE_REMOVE;
}

void on_call(FlMethodChannel* channel, FlMethodCall* call, gpointer data) {
  Test* test = static_cast<Test*>(data);
  if (g_strcmp0(fl_method_call_get_name(call), "start") != 0 || test->player ||
      test->step > 0) {
    fl_method_call_respond_not_implemented(call, nullptr);
    return;
  }
  show(test, play(test->file));
  g_timeout_add_seconds(1, step, test);
  g_autoptr(FlValue) id =
      fl_value_new_int(lychee_host_texture_get_id(test->texture));
  fl_method_call_respond_success(call, id, nullptr);
}

}  // namespace

void host_texture_test_start(FlPluginRegistry* registry) {
  const char* file = getenv("LYCHEE_HOST_TEXTURE_TEST");
  if (!file || !*file) {
    return;
  }
  g_autoptr(FlPluginRegistrar) registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "HostTextureTest");
  LycheeHostTexture* texture = lychee_host_texture_new(
      fl_plugin_registrar_get_texture_registrar(registrar));
  if (!texture) {
    fprintf(stderr, "host_texture_test: failed to create the texture\n");
    return;
  }
  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  Test* test = new Test{file, nullptr, texture, nullptr, 0, 0};
  test->channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "lychee_example/host_texture", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(test->channel, on_call, test,
                                            nullptr);
}
