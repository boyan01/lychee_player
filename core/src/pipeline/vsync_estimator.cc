#include "pipeline/vsync_estimator.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "base/time.h"

namespace lychee {

namespace {

// Until the caller's display times show otherwise.
constexpr int64_t kDefaultInterval = kMicrosPerSecond / 60;
// Samples in the fit: 10 s at 60 Hz. Jitter averages out over the window;
// cadence detection needs the interval within about 0.1%.
constexpr size_t kMaxSamples = 600;
// Calls this close together are the same vsync (acquire after has_new_frame).
constexpr int64_t kSameVsync = 2 * kMicrosPerMilli;
// A gap this long means the caller stopped pulling; the vsyncs in between are
// unknown, so start over.
constexpr int64_t kGap = 100 * kMicrosPerMilli;
// Recent samples whose mean distance from the grid tells a new refresh rate
// (the window moved to another screen) from jitter. Jitter up to J gives a
// mean of J/2; samples of another rate land anywhere between grid points,
// a mean near interval/4.
constexpr size_t kRecentSamples = 16;
// Samples kept after a refresh change, assumed one vsync apart. Detection
// needs at least this many at the new rate, even with jitter.
constexpr size_t kSamplesAfterChange = 4;

}  // namespace

VsyncEstimator::Vsync VsyncEstimator::Update(int64_t display_time) {
  if (!samples_.empty()) {
    const int64_t since_last = display_time - last_display_time_;
    if (since_last <= kSameVsync) {
      return last_;
    }
    if (since_last >= kGap) {
      samples_.clear();
      slope_ = 0;
      grid_valid_ = false;
      recent_errors_.clear();
      recent_error_sum_ = 0;
    }
  }
  last_display_time_ = display_time;

  int64_t index = 0;
  if (!samples_.empty()) {
    const Sample& last = samples_.back();
    const double interval = has_fit() ? slope_ : kDefaultInterval;
    const int64_t last_vsync = has_fit() ? TimeAt(last.index) : last.time;
    // More than one if the caller skipped vsyncs.
    const int64_t vsyncs = std::max<int64_t>(
        std::llround((display_time - last_vsync) / interval), 1);
    index = last.index + vsyncs;
    const int64_t error =
        has_fit() && vsyncs == 1
            ? std::min<int64_t>(std::llabs(display_time - TimeAt(index)),
                                std::llround(interval / 2))
            : std::llround(interval / 2);
    if (has_fit()) {
      recent_errors_.push_back(error);
      recent_error_sum_ += error;
      if (recent_errors_.size() > kRecentSamples) {
        recent_error_sum_ -= recent_errors_.front();
        recent_errors_.pop_front();
      }
    }
    if (recent_errors_.size() == kRecentSamples &&
        recent_error_sum_ > kRecentSamples * interval / 5) {
      samples_.erase(samples_.begin(),
                     samples_.end() -
                         std::min(samples_.size(), kSamplesAfterChange));
      for (size_t i = 0; i < samples_.size(); ++i) {
        samples_[i].index = static_cast<int64_t>(i);
      }
      index = static_cast<int64_t>(samples_.size());
      grid_valid_ = false;
      recent_errors_.clear();
      recent_error_sum_ = 0;
    }
  }
  samples_.push_back({index, display_time});
  if (samples_.size() > kMaxSamples) {
    samples_.pop_front();
  }
  Fit();

  // Each refit moves the line a little, most while samples are few. Advance
  // the output by exactly one interval per vsync and only steer it toward
  // the line, so consecutive vsyncs stay evenly spaced.
  const double interval = has_fit() ? slope_ : kDefaultInterval;
  const double target =
      has_fit() ? static_cast<double>(TimeAt(index)) : display_time;
  if (grid_valid_ && index > grid_index_) {
    const double predicted = grid_ + (index - grid_index_) * interval;
    const double error = target - predicted;
    grid_ = std::fabs(error) < interval / 4 ? predicted + error / 8 : target;
  } else {
    grid_ = target;
  }
  grid_index_ = index;
  grid_valid_ = true;
  last_ = {std::llround(grid_), std::llround(interval)};
  return last_;
}

void VsyncEstimator::Fit() {
  if (samples_.size() < 3) {
    slope_ = 0;
    return;
  }
  const Sample& first = samples_.front();
  const double n = static_cast<double>(samples_.size());
  double mean_x = 0;
  double mean_y = 0;
  for (const Sample& sample : samples_) {
    mean_x += static_cast<double>(sample.index - first.index);
    mean_y += static_cast<double>(sample.time - first.time);
  }
  mean_x /= n;
  mean_y /= n;
  double covariance = 0;
  double variance = 0;
  for (const Sample& sample : samples_) {
    const double dx = static_cast<double>(sample.index - first.index) - mean_x;
    const double dy = static_cast<double>(sample.time - first.time) - mean_y;
    covariance += dx * dy;
    variance += dx * dx;
  }
  slope_ = variance > 0 ? covariance / variance : 0;
  intercept_ = mean_y - slope_ * mean_x;
}

int64_t VsyncEstimator::TimeAt(int64_t index) const {
  const Sample& first = samples_.front();
  return first.time +
         std::llround(intercept_ +
                      slope_ * static_cast<double>(index - first.index));
}

}  // namespace lychee
