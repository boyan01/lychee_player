#include "pipeline/frame_rate_estimator.h"

#include <cmath>

namespace lychee {

namespace {

// Durations without a synced run after which the rate is variable.
constexpr int kUnsyncedLimit = 2 * FrameRateEstimator::kFramesToSync;

double Snap(double rate) {
  static constexpr double kRates[] = {24000.0 / 1001,
                                      24,
                                      25,
                                      30000.0 / 1001,
                                      30,
                                      48000.0 / 1001,
                                      48,
                                      50,
                                      60000.0 / 1001,
                                      60,
                                      100,
                                      120000.0 / 1001,
                                      120,
                                      144,
                                      240};
  for (double standard : kRates) {
    if (std::abs(rate - standard) <= standard * 0.0005) {
      return standard;
    }
  }
  return rate;
}

}  // namespace

void FrameRateEstimator::AddFrame(int64_t pts) {
  if (pts == kNoTimestamp) {
    return;
  }
  const int64_t last = last_pts_;
  last_pts_ = pts;
  if (last == kNoTimestamp) {
    return;
  }
  const int64_t duration = pts - last;
  bool matches = duration > 0;
  if (matches && run_frames_ > 0) {
    const int64_t mean = (last - run_start_) / run_frames_;
    matches = std::abs(duration - mean) <= kTolerance;
  }
  if (!matches || run_frames_ == 0) {
    if (run_frames_ >= kFramesToSync) {
      unsynced_ = 0;  // A synced run ended here.
    }
    run_start_ = duration > 0 ? last : pts;
    run_frames_ = duration > 0 ? 1 : 0;
  } else {
    ++run_frames_;
  }
  if (run_frames_ >= kFramesToSync) {
    frame_rate_ = Snap(static_cast<double>(run_frames_) * kMicrosPerSecond /
                       static_cast<double>(pts - run_start_));
  } else if (++unsynced_ > kUnsyncedLimit) {
    frame_rate_ = 0;
  }
}

void FrameRateEstimator::Restart() {
  last_pts_ = kNoTimestamp;
  run_start_ = kNoTimestamp;
  run_frames_ = 0;
  unsynced_ = 0;
}

}  // namespace lychee
