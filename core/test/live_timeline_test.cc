#include "pipeline/live_timeline.h"

#include <cstdlib>

#include "base/time.h"
#include "test.h"

namespace lychee {
namespace {

constexpr int64_t kMs = kMicrosPerMilli;
constexpr int64_t kLatency = 80 * kMs;
constexpr int64_t kFrame = 33 * kMs;

TEST(LiveTimeline, FirstPacketAnchorsAtLatency) {
  LiveTimeline timeline(kLatency);
  EXPECT_EQ(timeline.Video(5000 * kMs, 1000 * kMs), 1000 * kMs + kLatency);
  // Later packets keep the mapping.
  EXPECT_EQ(timeline.Video(5000 * kMs + kFrame, 1000 * kMs + kFrame),
            1000 * kMs + kFrame + kLatency);
}

TEST(LiveTimeline, ConvergesWhenTheFirstPacketWasSlow) {
  LiveTimeline timeline(kLatency);
  // The first packet took 100 ms longer than the rest.
  timeline.Video(0, 100 * kMs);
  int64_t headroom = 0;
  for (int64_t pts = kFrame; pts < 40000 * kMs; pts += kFrame) {
    headroom = timeline.Video(pts, pts) - pts;
  }
  EXPECT_TRUE(std::llabs(headroom - kLatency) <= 5 * kMs);
}

TEST(LiveTimeline, StepsAreBounded) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 1000 * kMs);
  int64_t previous = timeline.Video(kFrame, kFrame) - kFrame;
  for (int64_t pts = 2 * kFrame; pts < 10000 * kMs; pts += kFrame) {
    const int64_t headroom = timeline.Video(pts, pts) - pts;
    EXPECT_TRUE(std::llabs(headroom - previous) <= 50 * kMs);
    previous = headroom;
  }
}

TEST(LiveTimeline, RepeatedTimestampsDoNotAdapt) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 0);
  // An idle screen resends the last frame's timestamp long after.
  for (int64_t now = 0; now < 10000 * kMs; now += kFrame) {
    EXPECT_EQ(timeline.Video(0, now), kLatency);
  }
}

TEST(LiveTimeline, AudioFollowsVideoWhenTheDeviceLeadFits) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 0);
  EXPECT_EQ(timeline.Audio(10 * kMs, 20 * kMs, 10 * kMs), 10 * kMs + kLatency);
  EXPECT_EQ(timeline.audio_behind(), 0);
}

TEST(LiveTimeline, AudioPlaysLateForASlowDevice) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 0);
  // Bluetooth: the device takes audio 200 ms before it is heard.
  const int64_t lead = 200 * kMs;
  int64_t deadline = 0;
  for (int64_t pts = 0; pts < 3000 * kMs; pts += 10 * kMs) {
    deadline = timeline.Audio(pts, lead, pts);
  }
  // Enough lead for the device and the margin, still close to video.
  EXPECT_TRUE(deadline - 2990 * kMs >= lead);
  EXPECT_TRUE(timeline.audio_behind() <= lead + 30 * kMs - kLatency);
  // Video is unaffected.
  EXPECT_EQ(timeline.Video(3000 * kMs, 3000 * kMs), 3000 * kMs + kLatency);
}

TEST(LiveTimeline, AudioReturnsToVideoWhenTheLeadShrinks) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 0);
  int64_t pts = 0;
  for (; pts < 2000 * kMs; pts += 10 * kMs) {
    timeline.Audio(pts, 200 * kMs, pts);
  }
  for (; pts < 6000 * kMs; pts += 10 * kMs) {
    timeline.Audio(pts, 10 * kMs, pts);
  }
  EXPECT_TRUE(timeline.audio_behind() <= 20 * kMs);
}

TEST(LiveTimeline, AudioStaysAtMostHalfASecondBehind) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 0);
  for (int64_t pts = 0; pts < 2000 * kMs; pts += 10 * kMs) {
    timeline.Audio(pts, 2000 * kMs, pts);
  }
  EXPECT_EQ(timeline.audio_behind(), 500 * kMs);
}

TEST(LiveTimeline, ResetAnchorsAgain) {
  LiveTimeline timeline(kLatency);
  timeline.Video(0, 0);
  timeline.Reset();
  EXPECT_EQ(timeline.Video(0, 7000 * kMs), 7000 * kMs + kLatency);
}

}  // namespace
}  // namespace lychee
