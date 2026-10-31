#include "media/audio_buffer.h"

#include "base/time.h"
#include "test.h"

namespace lychee {
namespace {

constexpr int64_t kMs = kMicrosPerMilli;
constexpr int64_t kPacket = 21 * kMs;

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(AudioTimestampFollower, KeepsTheDecoderDelay) {
  AudioTimestampFollower follower;
  // The counted output is 2 ms behind the input throughout.
  for (int64_t pts = 0; pts < 1000 * kMs; pts += kPacket) {
    EXPECT_EQ(follower.Follow(pts, pts - 2 * kMs), 0);
  }
}

TEST(AudioTimestampFollower, FollowsJumps) {
  AudioTimestampFollower follower;
  int64_t shift = 0;
  int64_t counted = 0;
  for (int64_t pts = 0; pts < 2000 * kMs; pts += kPacket) {
    // A second with no audio, then a timeline moving audio back.
    const int64_t input = pts + (pts >= 1000 * kMs ? 1000 * kMs : 0) -
                          (pts >= 1500 * kMs ? 30 * kMs : 0);
    shift += follower.Follow(input, counted + shift);
    EXPECT_EQ(counted + shift, input);
    counted += kPacket;
  }
}

TEST(AudioTimestampFollower, LeavesDriftAlone) {
  AudioTimestampFollower follower;
  // Decoded at the wrong rate: the count gains 2 ms a packet.
  int64_t counted = 0;
  for (int64_t pts = 0; pts < 2000 * kMs; pts += kPacket) {
    EXPECT_EQ(follower.Follow(pts, counted), 0);
    counted += kPacket + 2 * kMs;
  }
}
// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
