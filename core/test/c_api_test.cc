#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "base/log.h"
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

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(CApi, CodecSupported) {
  for (int32_t codec : {LYCHEE_CODEC_H264, LYCHEE_CODEC_HEVC, LYCHEE_CODEC_AAC,
                        LYCHEE_CODEC_ALAC}) {
    for (int32_t hardware : {0, 1}) {
      const int32_t supported = lychee_codec_supported(codec, hardware);
      EXPECT_TRUE(supported >= -1 && supported <= 1);
    }
  }
  EXPECT_EQ(lychee_codec_supported(LYCHEE_CODEC_NONE, 0), 0);
  EXPECT_EQ(lychee_codec_supported(99, 1), 0);
#if defined(__APPLE__)
  // VideoToolbox and AudioToolbox, or FFmpeg, decode them all.
  EXPECT_EQ(lychee_codec_supported(LYCHEE_CODEC_H264, 0), 1);
  EXPECT_EQ(lychee_codec_supported(LYCHEE_CODEC_HEVC, 0), 1);
  EXPECT_EQ(lychee_codec_supported(LYCHEE_CODEC_AAC, 0), 1);
  EXPECT_EQ(lychee_codec_supported(LYCHEE_CODEC_ALAC, 1), 1);
#endif
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

TEST(CApi, NotSeekableBeforeOpen) {
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = 0;
  lychee_player* player = lychee_player_create(&config);
  EXPECT_EQ(lychee_player_is_seekable(player), 0);
  lychee_player_destroy(player);
}

// Fails the run instead of hanging it when a setter deadlocks.
class Watchdog {
 public:
  explicit Watchdog(const char* what) {
    thread_ = std::thread([this, what] {
      const auto give_up =
          std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (!done_ && std::chrono::steady_clock::now() < give_up) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!done_) {
        std::fprintf(stderr, "%s: deadlocked\n", what);
        std::_Exit(1);
      }
    });
  }
  ~Watchdog() {
    done_ = true;
    thread_.join();
  }

 private:
  std::atomic<bool> done_{false};
  std::thread thread_;
};

void WaitFor(const std::atomic<int>& value, int at_least) {
  while (value < at_least) {
    std::this_thread::yield();
  }
}

struct SwapSinks {
  std::atomic<int> inside{0};
  std::atomic<int> swapped{0};
};
SwapSinks g_swap;

void SwapToB(void*, int32_t, const char*);

// Both threads are in a callback before either replaces it, so each
// setter runs while the other thread is still inside.
void SwapToA(void*, int32_t, const char*) {
  g_swap.inside++;
  WaitFor(g_swap.inside, 2);
  lychee_set_log_callback(&SwapToB, nullptr);
  g_swap.swapped++;
  WaitFor(g_swap.swapped, 2);
}

void SwapToB(void*, int32_t, const char*) {}

TEST(CApi, LogCallbacksReplaceEachOther) {
  Watchdog watchdog("LogCallbacksReplaceEachOther");
  lychee_set_log_callback(&SwapToA, nullptr);
  std::thread first([] { LogMessage(LogLevel::kError, "test", "first"); });
  std::thread second([] { LogMessage(LogLevel::kError, "test", "second"); });
  first.join();
  second.join();
  EXPECT_EQ(g_swap.swapped.load(), 2);
  lychee_set_log_callback(nullptr, nullptr);
}

std::atomic<int> g_blocked_inside{0};
std::atomic<bool> g_release_blocked{false};

void BlockingLogCallback(void*, int32_t, const char*) {
  g_blocked_inside++;
  while (!g_release_blocked) {
    std::this_thread::yield();
  }
}

// From outside a callback the setter still waits for the calls in flight.
TEST(CApi, LogCallbackSetterWaitsOutsideCallbacks) {
  Watchdog watchdog("LogCallbackSetterWaitsOutsideCallbacks");
  lychee_set_log_callback(&BlockingLogCallback, nullptr);
  std::thread logger([] { LogMessage(LogLevel::kError, "test", "blocked"); });
  WaitFor(g_blocked_inside, 1);
  std::atomic<bool> returned{false};
  std::thread setter([&returned] {
    lychee_set_log_callback(nullptr, nullptr);
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(!returned);
  g_release_blocked = true;
  setter.join();
  logger.join();
  EXPECT_TRUE(returned);
}

struct BlockedEvents {
  std::atomic<int> inside{0};
  std::atomic<bool> release{false};
};

void BlockingEventCallback(void* user_data, int32_t, int64_t, int64_t) {
  auto* test = static_cast<BlockedEvents*>(user_data);
  test->inside++;
  while (!test->release) {
    std::this_thread::yield();
  }
}

// Clearing the callback must wait for an event being handed to it: a
// binding frees what the callback is bound to right after (a Dart isolate
// a hot restart ends runs its finalizers, then deletes its callbacks).
TEST(CApi, ClearEventCallbackWaitsForTheCallback) {
  Watchdog watchdog("ClearEventCallbackWaitsForTheCallback");
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = 0;
  lychee_player* player = lychee_player_create(&config);
  BlockedEvents test;
  lychee_player_set_event_callback(player, &BlockingEventCallback, &test);
  lychee_player_open(player, "/nonexistent/d.mp4");
  WaitFor(test.inside, 1);
  std::atomic<bool> cleared{false};
  std::thread clear([&] {
    lychee_player_clear_event_callback(player);
    cleared = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(!cleared.load());
  test.release = true;
  clear.join();
  EXPECT_TRUE(cleared.load());
  const int seen = test.inside;
  lychee_player_open(player, "/nonexistent/e.mp4");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  lychee_player_destroy(player);
  EXPECT_EQ(test.inside.load(), seen);
}

void ClearFromCallback(void* user_data, int32_t, int64_t, int64_t) {
  auto* player = static_cast<lychee_player*>(user_data);
  lychee_player_clear_event_callback(player);
}

// From inside its own callback, the setter must not wait for itself.
TEST(CApi, ClearEventCallbackFromTheCallback) {
  Watchdog watchdog("ClearEventCallbackFromTheCallback");
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = 0;
  lychee_player* player = lychee_player_create(&config);
  lychee_player_set_event_callback(player, &ClearFromCallback, player);
  lychee_player_open(player, "/nonexistent/f.mp4");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  lychee_player_destroy(player);
}

}  // namespace
}  // namespace lychee
