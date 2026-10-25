#include "pipeline/media_clock.h"

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

}  // namespace
}  // namespace lychee
