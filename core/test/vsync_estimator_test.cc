#include "pipeline/vsync_estimator.h"

#include <algorithm>
#include <cstdlib>
#include <random>

#include "test.h"

namespace lychee {
namespace {

constexpr int64_t kStart = 1000000000;

// Feeds |count| vsyncs of |interval| starting at |*vsync| with uniform jitter
// up to |jitter|, and returns the largest distance of a snapped time from
// the true vsync over the last |checked| of them.
int64_t Feed(VsyncEstimator* estimator, int64_t* vsync, double interval,
             int count, int64_t jitter, int checked, int64_t* last_interval,
             std::mt19937* rng) {
  // std::mt19937's output is specified, its distributions are not: map it by
  // hand so every standard library sees the same jitter.
  auto noise = [&] {
    return static_cast<int64_t>((*rng)() % (2 * jitter + 1)) - jitter;
  };
  int64_t worst = 0;
  double exact = static_cast<double>(*vsync);
  for (int i = 0; i < count; ++i) {
    const int64_t truth = std::llround(exact);
    VsyncEstimator::Vsync result = estimator->Update(truth + noise());
    if (i >= count - checked) {
      worst = std::max(worst, std::abs(result.time - truth));
    }
    *last_interval = result.interval;
    exact += interval;
  }
  *vsync = std::llround(exact);
  return worst;
}

TEST(VsyncEstimator, ExactTimesStayExact) {
  VsyncEstimator estimator;
  std::mt19937 rng(1);
  int64_t vsync = kStart;
  int64_t interval = 0;
  EXPECT_TRUE(
      Feed(&estimator, &vsync, 16666.67, 100, 0, 97, &interval, &rng) <= 1);
  EXPECT_NEAR(interval, 16667, 1);
}

TEST(VsyncEstimator, RemovesJitter) {
  for (unsigned seed = 1; seed <= 20; ++seed) {
    VsyncEstimator estimator;
    std::mt19937 rng(seed);
    int64_t vsync = kStart;
    int64_t interval = 0;
    // 4 ms of jitter on 60 Hz: after two seconds, snapped times are within
    // 1 ms (the fitted line's uncertainty), and the interval within 0.1%.
    // The jitter must not look like a refresh change.
    const int64_t worst =
        Feed(&estimator, &vsync, 16666.67, 600, 4000, 480, &interval, &rng);
    EXPECT_TRUE(worst < 1000);
    EXPECT_NEAR(interval, 16667, 17);
  }
}

TEST(VsyncEstimator, SameVsyncGetsSameAnswer) {
  VsyncEstimator estimator;
  std::mt19937 rng(1);
  int64_t vsync = kStart;
  int64_t interval = 0;
  Feed(&estimator, &vsync, 16666.67, 30, 1000, 0, &interval, &rng);
  VsyncEstimator::Vsync first = estimator.Update(vsync);
  VsyncEstimator::Vsync again = estimator.Update(vsync + 500);
  EXPECT_EQ(first.time, again.time);
  EXPECT_EQ(first.interval, again.interval);
}

TEST(VsyncEstimator, SkippedVsyncsKeepTheGrid) {
  VsyncEstimator estimator;
  std::mt19937 rng(1);
  int64_t vsync = kStart;
  int64_t interval = 0;
  Feed(&estimator, &vsync, 16666.67, 120, 2000, 0, &interval, &rng);
  // The caller misses two vsyncs.
  vsync += 2 * 16667;
  const int64_t worst =
      Feed(&estimator, &vsync, 16666.67, 30, 2000, 30, &interval, &rng);
  EXPECT_TRUE(worst < 500);
  EXPECT_NEAR(interval, 16667, 17);
}

TEST(VsyncEstimator, FollowsRefreshRateChange) {
  for (double next : {8333.33, 33333.33, 20000.0}) {
    VsyncEstimator estimator;
    std::mt19937 rng(1);
    int64_t vsync = kStart;
    int64_t interval = 0;
    Feed(&estimator, &vsync, 16666.67, 300, 1000, 0, &interval, &rng);
    // The window moves to another screen. A second later the estimate
    // matches it.
    const int count = static_cast<int>(std::llround(2e6 / next));
    const int64_t worst = Feed(&estimator, &vsync, next, count, 1000,
                               count / 2, &interval, &rng);
    EXPECT_TRUE(worst < 500);
    EXPECT_NEAR(interval, std::llround(next), std::llround(next / 100));
  }
}

TEST(VsyncEstimator, StartsOverAfterGap) {
  VsyncEstimator estimator;
  std::mt19937 rng(1);
  int64_t vsync = kStart;
  int64_t interval = 0;
  Feed(&estimator, &vsync, 16666.67, 60, 1000, 0, &interval, &rng);
  // Not pulled for a second, then resumed off the old grid.
  vsync += 1000000 + 7000;
  const int64_t worst =
      Feed(&estimator, &vsync, 16666.67, 60, 1000, 30, &interval, &rng);
  EXPECT_TRUE(worst < 500);
}

}  // namespace
}  // namespace lychee
