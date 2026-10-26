#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>

#include "lychee/lychee.h"
#include "test.h"

namespace lychee {
namespace {

// A caller built against an older header: its struct ends early, and the
// library must not write past it.
constexpr unsigned char kGuard = 0xa5;

bool GuardIntact(const unsigned char* bytes, size_t from, size_t to) {
  for (size_t i = from; i < to; ++i) {
    if (bytes[i] != kGuard) {
      return false;
    }
  }
  return true;
}

TEST(CApi, ConfigInitWritesOnlyTheCallersSize) {
  alignas(lychee_config) unsigned char bytes[sizeof(lychee_config) + 16];
  memset(bytes, kGuard, sizeof(bytes));
  const uint32_t old_size = offsetof(lychee_config, video_device);
  lychee_config_init(reinterpret_cast<lychee_config*>(bytes), old_size);
  const auto* config = reinterpret_cast<const lychee_config*>(bytes);
  EXPECT_EQ(config->struct_size, old_size);
  EXPECT_EQ(config->hardware_decoding, 1);
  EXPECT_TRUE(GuardIntact(bytes, old_size, sizeof(bytes)));

  lychee_config full;
  lychee_config_init(&full, sizeof(full));
  EXPECT_EQ(full.struct_size, sizeof(full));
  EXPECT_TRUE(full.video_device == nullptr);
}

TEST(CApi, GettersKeepTheCallersSize) {
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = 0;
  lychee_player* player = lychee_player_create(&config);

  alignas(lychee_stats) unsigned char bytes[sizeof(lychee_stats) + 16];
  memset(bytes, kGuard, sizeof(bytes));
  const uint32_t old_size = offsetof(lychee_stats, video_buffered_us);
  auto* stats = reinterpret_cast<lychee_stats*>(bytes);
  stats->struct_size = old_size;
  // Twice: the first call must leave a size the second still honours.
  lychee_player_get_stats(player, stats);
  lychee_player_get_stats(player, stats);
  EXPECT_EQ(stats->struct_size, old_size);
  EXPECT_TRUE(GuardIntact(bytes, old_size, sizeof(bytes)));

  lychee_player_destroy(player);
}

struct ReopenFromCallback {
  lychee_player* player = nullptr;
  std::atomic<int> result{1};
};

void Reopen(void* user_data, int32_t type, int64_t, int64_t) {
  auto* test = static_cast<ReopenFromCallback*>(user_data);
  if (type == LYCHEE_EVENT_STATE_CHANGED && test->result == 1) {
    test->result = lychee_player_open(test->player, "/nonexistent/b.mp4");
  }
}

// An event callback that opens the next file must get an error, not have
// the open join the thread the callback runs on.
TEST(CApi, OpenFromEventCallbackFails) {
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = 0;
  ReopenFromCallback test;
  test.player = lychee_player_create(&config);
  lychee_player_set_event_callback(test.player, &Reopen, &test);
  EXPECT_EQ(lychee_player_open(test.player, "/nonexistent/a.mp4"), 0);
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (test.result == 1 && std::chrono::steady_clock::now() < give_up) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(test.result.load(), -1);
  lychee_player_destroy(test.player);
}

std::atomic<int> g_log_lines{0};

void ReplaceLogCallback(void*, int32_t, const char*) {
  g_log_lines++;
  lychee_set_log_callback(nullptr, nullptr);
}

// A log callback that replaces itself must not deadlock on the library's
// log lock.
TEST(CApi, LogCallbackReplacesItself) {
  lychee_set_log_callback(&ReplaceLogCallback, nullptr);
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = 0;
  lychee_player* player = lychee_player_create(&config);
  lychee_player_open(player, "/nonexistent/c.mp4");
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (g_log_lines == 0 && std::chrono::steady_clock::now() < give_up) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  lychee_player_destroy(player);
  lychee_set_log_callback(nullptr, nullptr);
  EXPECT_TRUE(g_log_lines >= 1);
}

}  // namespace
}  // namespace lychee
