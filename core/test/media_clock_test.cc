#include "pipeline/media_clock.h"

#include <algorithm>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

constexpr int64_t kSecond = kMicrosPerSecond;
constexpr int64_t kMs = kMicrosPerMilli;

TEST(MediaClock, PausedClockStaysAtResetTime) {
  MediaClock clock;
  clock.Reset(5 * kSecond);
  EXPECT_TRUE(!clock.playing());
  EXPECT_EQ(clock.MediaTimeAt(0), 5 * kSecond);
  EXPECT_EQ(clock.MediaTimeAt(NowMicros() + 10 * kSecond), 5 * kSecond);
}

TEST(MediaClock, AudioDrivesClockUntilMaxTime) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play();
  const int64_t heard_at = NowMicros();
  clock.UpdateFromAudio(1 * kSecond, heard_at, 1500 * kMs);
  EXPECT_EQ(clock.MediaTimeAt(heard_at + 200 * kMs), 1200 * kMs);
  // An underrun: the clock stops at the end of the audio handed over.
  EXPECT_EQ(clock.MediaTimeAt(heard_at + 900 * kMs), 1500 * kMs);
}

TEST(MediaClock, IgnoresAudioWhilePaused) {
  MediaClock clock;
  clock.Reset(2 * kSecond);
  clock.UpdateFromAudio(9 * kSecond, NowMicros(), kInfiniteTime);
  EXPECT_EQ(clock.MediaTimeAt(NowMicros()), 2 * kSecond);
}

TEST(MediaClock, PauseKeepsPosition) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play();
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(3 * kSecond, now, kInfiniteTime);
  clock.Pause();
  const int64_t paused_at = clock.MediaTimeAt(0);
  EXPECT_TRUE(paused_at >= 3 * kSecond);
  EXPECT_EQ(clock.MediaTimeAt(NowMicros() + kSecond), paused_at);
}

TEST(MediaClock, ClearMaxTimeContinuesFromStall) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play();
  // Audio ended a second ago at 4 s; the clock stalled there.
  clock.UpdateFromAudio(3 * kSecond, NowMicros() - 2 * kSecond, 4 * kSecond);
  clock.ClearMaxTime();
  EXPECT_NEAR(clock.Now(), 4 * kSecond, 50 * kMs);
}

TEST(MediaClock, SystemTimesForMapsMediaTimes) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play();
  const int64_t heard_at = NowMicros();
  clock.UpdateFromAudio(1 * kSecond, heard_at, kInfiniteTime);
  std::vector<int64_t> system_times;
  EXPECT_TRUE(clock.SystemTimesFor({1 * kSecond, 1100 * kMs}, &system_times));
  EXPECT_EQ(system_times.size(), 2u);
  EXPECT_EQ(system_times[0], heard_at);
  EXPECT_EQ(system_times[1], heard_at + 100 * kMs);
}

TEST(MediaClock, SystemTimesForWhilePausedStartFromNow) {
  MediaClock clock;
  clock.Reset(1 * kSecond);
  std::vector<int64_t> system_times;
  const int64_t before = NowMicros();
  EXPECT_TRUE(!clock.SystemTimesFor({1 * kSecond}, &system_times));
  EXPECT_NEAR(system_times[0], before, 50 * kMs);
}

TEST(MediaClock, RateScalesMediaTime) {
  MediaClock clock;
  clock.Reset(0);
  clock.SetRate(2);
  clock.Play();
  const int64_t heard_at = NowMicros();
  clock.UpdateFromAudio(1 * kSecond, heard_at, kInfiniteTime);
  EXPECT_EQ(clock.MediaTimeAt(heard_at + 100 * kMs), 1200 * kMs);
  std::vector<int64_t> system_times;
  clock.SystemTimesFor({1 * kSecond, 1200 * kMs}, &system_times);
  EXPECT_EQ(system_times[0], heard_at);
  EXPECT_EQ(system_times[1], heard_at + 100 * kMs);
}

TEST(MediaClock, SetRateContinuesFromCurrentTime) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play();
  clock.UpdateFromAudio(5 * kSecond, NowMicros(), kInfiniteTime);
  const int64_t before = clock.Now();
  clock.SetRate(0.5);
  EXPECT_EQ(clock.rate(), 0.5);
  const int64_t now = NowMicros();
  const int64_t at_now = clock.MediaTimeAt(now);
  EXPECT_NEAR(at_now, before, 5 * kMs);
  EXPECT_NEAR(clock.MediaTimeAt(now + kSecond), at_now + kSecond / 2, 1);
  // Ignored.
  clock.SetRate(0);
  EXPECT_EQ(clock.rate(), 0.5);
}

TEST(MediaClock, WaitsForAudioBeforeMoving) {
  MediaClock clock;
  clock.Reset(7 * kSecond);
  clock.Play(true);
  EXPECT_TRUE(clock.playing());
  const int64_t now = NowMicros();
  EXPECT_EQ(clock.MediaTimeAt(now + 500 * kMs), 7 * kSecond);
  std::vector<int64_t> system_times;
  EXPECT_TRUE(!clock.SystemTimesFor({7 * kSecond}, &system_times));

  // The first buffer is heard 150 ms from now. Time stays at the start
  // until then instead of running back.
  const int64_t heard_at = NowMicros() + 150 * kMs;
  clock.UpdateFromAudio(7 * kSecond, heard_at, kInfiniteTime);
  EXPECT_TRUE(clock.SystemTimesFor({7 * kSecond}, &system_times));
  EXPECT_EQ(system_times[0], heard_at);
  EXPECT_EQ(clock.MediaTimeAt(heard_at - 100 * kMs), 7 * kSecond);
  EXPECT_EQ(clock.MediaTimeAt(heard_at + 100 * kMs), 7100 * kMs);
}

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)

// A route change can discard 200 ms already handed to Bluetooth and then
// ask for the next samples on a low-latency speaker. The video clock must
// approach that new audio position without skipping the entire gap.
TEST(MediaClock, RouteChangeDoesNotStepForward) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(kSecond, now + 200 * kMs, 1010 * kMs);
  const int64_t before = clock.MediaTimeAt(now);
  clock.UpdateFromAudio(1010 * kMs, now + 10 * kMs, 1020 * kMs);
  EXPECT_NEAR(clock.MediaTimeAt(now), before, kMs);
}

TEST(MediaClock, RouteChangeDoesNotStepBackward) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(kSecond, now + 10 * kMs, 1010 * kMs);
  const int64_t before = clock.MediaTimeAt(now);
  clock.UpdateFromAudio(1010 * kMs, now + 220 * kMs, 1020 * kMs);
  EXPECT_NEAR(clock.MediaTimeAt(now), before, kMs);
}

TEST(MediaClock, RouteCorrectionConvergesWithoutPersistentOffset) {
  for (double rate : {0.5, 1.0, 2.0}) {
    for (int direction : {-1, 1}) {
      MediaClock clock;
      clock.Reset(0);
      clock.SetRate(rate);
      clock.Play(true);
      const int64_t now = NowMicros();
      const auto span = static_cast<int64_t>(10 * kMs * rate);
      clock.UpdateFromAudio(2 * kSecond, now + 300 * kMs, 2 * kSecond + span);
      const int64_t before = clock.MediaTimeAt(now);
      const auto step = static_cast<int64_t>(direction * kMs * rate);
      // Supply contiguous 10 ms buffers; only the device's latency changes.
      // The allowance scales with playback rate, like the gap.
      for (int i = 1; i <= 201; ++i) {
        clock.UpdateFromAudio(2 * kSecond + i * span,
                              now + (300 + i * 10 - direction * 200) * kMs,
                              2 * kSecond + (i + 1) * span);
        EXPECT_EQ(clock.MediaTimeAt(now), before + std::min(i, 200) * step);
      }
    }
  }
}

TEST(MediaClock, SampleDiscontinuityIsNotMistakenForDeviceLatency) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(kSecond, now + 200 * kMs, 1010 * kMs);
  // A fragmented file can jump its PTS at the loop boundary. The samples
  // themselves advanced 40 ms; preserving the old timeline would make
  // video temporarily lag the samples on every loop.
  clock.UpdateFromAudio(1050 * kMs, now + 210 * kMs, 1060 * kMs);
  EXPECT_EQ(clock.MediaTimeAt(now), 840 * kMs);
}

TEST(MediaClock, FreshPlaybackDoesNotSlew) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(kSecond, now, 3 * kSecond);
  clock.Pause();
  clock.Play(true);
  clock.UpdateFromAudio(2 * kSecond, now, 3 * kSecond);
  EXPECT_EQ(clock.MediaTimeAt(now), 2 * kSecond);
  clock.Reset(5 * kSecond);
  clock.Play(true);
  clock.UpdateFromAudio(5 * kSecond, now + 200 * kMs, 6 * kSecond);
  EXPECT_EQ(clock.MediaTimeAt(now + 300 * kMs), 5100 * kMs);
}

TEST(MediaClock, AudioAfterUnderrunStartsAtItsActualPresentationTime) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(kSecond, now - kSecond, 1100 * kMs);
  EXPECT_EQ(clock.MediaTimeAt(now), 1100 * kMs);
  clock.UpdateFromAudio(1100 * kMs, now, 1200 * kMs);
  EXPECT_EQ(clock.MediaTimeAt(now), 1100 * kMs);
  EXPECT_EQ(clock.MediaTimeAt(now + 50 * kMs), 1150 * kMs);
}

TEST(MediaClock, StartupLatencyIsNotMistakenForClockError) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  const int64_t now = NowMicros();
  clock.UpdateFromAudio(0, now + 200 * kMs, 10 * kMs);
  clock.UpdateFromAudio(10 * kMs, now + 210 * kMs, 20 * kMs);
  EXPECT_EQ(clock.MediaTimeAt(now + 205 * kMs), 5 * kMs);
}

// NOLINTEND(bugprone-throwing-static-initialization)

// A device that never pulls is the audio renderer's to replace: the clock
// does not run off on its own, or it would jump back when audio comes.
TEST(MediaClock, WaitsForAudioWithoutTimeout) {
  MediaClock clock;
  clock.Reset(0);
  clock.Play(true);
  EXPECT_EQ(clock.MediaTimeAt(NowMicros() + 10 * kSecond), 0);
  // Paused and played again, it still waits.
  clock.Pause();
  clock.Play(true);
  EXPECT_EQ(clock.MediaTimeAt(NowMicros() + 10 * kSecond), 0);
}

TEST(MediaClock, EndedAudioStopsTheWait) {
  MediaClock clock;
  clock.Reset(3 * kSecond);
  clock.Play(true);
  clock.ClearMaxTime();
  std::vector<int64_t> system_times;
  EXPECT_TRUE(clock.SystemTimesFor({3 * kSecond}, &system_times));
  EXPECT_NEAR(clock.MediaTimeAt(NowMicros() + kSecond), 4 * kSecond, 5 * kMs);
}

}  // namespace
}  // namespace lychee
