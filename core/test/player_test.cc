#include <atomic>
#include <thread>

#include "pipeline/player.h"
#include "test.h"

namespace lychee {
namespace {

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(Player, StatsDuringClose) {
  Player player({});
  std::atomic<bool> started{false};
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    do {
      player.stats();
      player.playing_variant();
      started = true;
    } while (!stop);
  });
  while (!started) {
    std::this_thread::yield();
  }
  for (int i = 0; i < 12; ++i) {
    EXPECT_TRUE(player.OpenLive(0));
    EXPECT_TRUE(player.Close());
  }
  stop = true;
  reader.join();
  EXPECT_EQ(player.stats().frames_rendered, 0);
  EXPECT_EQ(player.playing_variant(), -1);
}

}  // namespace
}  // namespace lychee
