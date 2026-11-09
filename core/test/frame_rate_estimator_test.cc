#include "pipeline/frame_rate_estimator.h"

#include <cmath>
#include <cstdint>

#include "test.h"

namespace lychee {
namespace {

// Feeds |count| frames at |fps| from |*frame| on, with display times
// rounded to |quantum| microseconds, as a container's timebase stores
// them.
void Feed(FrameRateEstimator* estimator,
          double fps,
          int count,
          int64_t quantum,
          int* frame) {
  for (int i = 0; i < count; ++i, ++*frame) {
    const double exact = *frame * 1e6 / fps;
    estimator->AddFrame(std::llround(exact / static_cast<double>(quantum)) *
                        quantum);
  }
}

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(FrameRateEstimator, SyncsOnSteadyDurations) {
  FrameRateEstimator estimator;
  int frame = 0;
  Feed(&estimator, 25, FrameRateEstimator::kFramesToSync - 1, 1, &frame);
  EXPECT_TRUE(estimator.frame_rate() == 0);
  Feed(&estimator, 25, 2, 1, &frame);
  EXPECT_TRUE(estimator.frame_rate() == 25);
}

TEST(FrameRateEstimator, SnapsFractionalRates) {
  // MPEG-TS's 90 kHz: exact from the start.
  FrameRateEstimator ts;
  int frame = 0;
  Feed(&ts, 24000.0 / 1001, 20, 1, &frame);
  EXPECT_TRUE(ts.frame_rate() == 24000.0 / 1001);
  // Milliseconds (FLV): 41 and 42 ms, which a longer run tells from 24.
  FrameRateEstimator flv;
  frame = 0;
  Feed(&flv, 24000.0 / 1001, 120, 1000, &frame);
  EXPECT_TRUE(flv.frame_rate() == 24000.0 / 1001);
  FrameRateEstimator flv24;
  frame = 0;
  Feed(&flv24, 24, 120, 1000, &frame);
  EXPECT_TRUE(flv24.frame_rate() == 24);
  FrameRateEstimator ntsc;
  frame = 0;
  Feed(&ntsc, 60000.0 / 1001, 200, 1000, &frame);
  EXPECT_TRUE(ntsc.frame_rate() == 60000.0 / 1001);
}

TEST(FrameRateEstimator, LeavesOtherRatesUnsnapped) {
  FrameRateEstimator estimator;
  int frame = 0;
  Feed(&estimator, 15, 40, 1, &frame);
  EXPECT_TRUE(std::abs(estimator.frame_rate() - 15) < 1e-9);
}

TEST(FrameRateEstimator, KeepsTheRateAcrossASeek) {
  FrameRateEstimator estimator;
  int frame = 0;
  Feed(&estimator, 50, 30, 1, &frame);
  EXPECT_TRUE(estimator.frame_rate() == 50);
  estimator.Restart();
  frame = 1000;
  Feed(&estimator, 50, 3, 1, &frame);
  EXPECT_TRUE(estimator.frame_rate() == 50);
  // A new rate (another variant) replaces it once it syncs.
  estimator.Restart();
  Feed(&estimator, 25, 20, 1, &frame);
  EXPECT_TRUE(estimator.frame_rate() == 25);
}

TEST(FrameRateEstimator, HasNoneForVariableFrameRate) {
  FrameRateEstimator estimator;
  int frame = 0;
  Feed(&estimator, 30, 20, 1, &frame);
  EXPECT_TRUE(estimator.frame_rate() == 30);
  // Durations of 20, 33 and 50 ms in turn never settle.
  int64_t pts = 10000000;
  static constexpr int64_t kDurations[] = {20000, 33000, 50000};
  for (int i = 0; i < 60; ++i) {
    pts += kDurations[i % 3];
    estimator.AddFrame(pts);
  }
  EXPECT_TRUE(estimator.frame_rate() == 0);
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
