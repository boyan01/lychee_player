// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from Chromium's media/filters/video_cadence_estimator.cc.

#define LOG_TAG "cadence"

#include "pipeline/video_cadence_estimator.h"

#include <cmath>
#include <cstdlib>
#include <string>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

// To prevent oscillation in and out of cadence or between cadence values, a
// cadence switch is accepted only after this much time.
constexpr int64_t kMinimumCadenceDuration = 100 * kMicrosPerMilli;

// The ratio of frame duration deviation to render interval above which the
// content counts as variable frame rate, and below which it counts as
// constant again; in between, the previous decision stands.
constexpr double kVariableFPSFactor = 0.55;
constexpr double kConstantFPSFactor = 0.45;

// Cadence is not used for frames longer than this (below 10 fps).
constexpr int64_t kMaxCadenceFrameDuration = 100 * kMicrosPerMilli;

// The longest cadence pattern tried. Enough for standard frame and refresh
// rates, short enough that timing errors do not make it useless.
constexpr int kMaxCadenceSize = 5;

// A cadence of |n| entries summing to |k|, each floor(k/n) or floor(k/n)+1,
// spread as evenly as possible.
VideoCadenceEstimator::Cadence ConstructCadence(int k, int n) {
  const int quotient = k / n;
  VideoCadenceEstimator::Cadence output(n, 0);
  // Integer arithmetic on accumulations scaled by |n|.
  int target_accumulate = 0;
  int actual_accumulate = 0;
  for (int i = 0; i < n; ++i) {
    target_accumulate += k;
    const int target_current = target_accumulate - actual_accumulate;
    // With k < n the pattern starts with 1, so the first frame is shown.
    if ((i == 0 && k < n) || target_current * 2 >= n * (quotient * 2 + 1)) {
      output[i] = quotient + 1;
    } else {
      output[i] = quotient;
    }
    actual_accumulate += output[i] * n;
  }
  return output;
}

VideoCadenceEstimator::Cadence ConstructSimpleCadence(
    double perfect_cadence,
    int64_t max_acceptable_drift,
    int64_t* time_until_max_drift) {
  int cadence_value = static_cast<int>(std::round(perfect_cadence));
  if (cadence_value < 0) {
    return {};
  }
  if (cadence_value == 0) {
    cadence_value = 1;
  }
  // Equivalent to render_interval * max_acceptable_drift /
  // |frame_duration - render_interval|.
  const double error = std::fabs(1.0 - perfect_cadence / cadence_value);
  *time_until_max_drift =
      error > 0 ? static_cast<int64_t>(max_acceptable_drift / error)
                : kInfiniteTime;
  return ConstructCadence(cadence_value, 1);
}

std::string CadenceToString(const VideoCadenceEstimator::Cadence& cadence) {
  std::string result = "[";
  for (size_t i = 0; i < cadence.size(); ++i) {
    result += (i ? ":" : "") + std::to_string(cadence[i]);
  }
  return result + "]";
}

}  // namespace

VideoCadenceEstimator::VideoCadenceEstimator(
    int64_t minimum_time_until_max_drift)
    : cadence_hysteresis_threshold_(kMinimumCadenceDuration),
      minimum_time_until_max_drift_(minimum_time_until_max_drift) {
  Reset();
}

void VideoCadenceEstimator::Reset() {
  cadence_.clear();
  pending_cadence_.clear();
  render_intervals_cadence_held_ = 0;
}

bool VideoCadenceEstimator::UpdateCadenceEstimate(
    int64_t render_interval,
    int64_t frame_duration,
    int64_t frame_duration_deviation,
    int64_t max_acceptable_drift) {
  constexpr double kRenderIntervalChangeThreshold = 0.1;
  const bool render_interval_changed =
      last_render_interval_ > 0 &&
      std::llabs(last_render_interval_ - render_interval) >
          last_render_interval_ * kRenderIntervalChangeThreshold;
  last_render_interval_ = render_interval;

  if (frame_duration_deviation > kVariableFPSFactor * render_interval) {
    is_variable_frame_rate_ = true;
  } else if (frame_duration_deviation < kConstantFPSFactor * render_interval) {
    is_variable_frame_rate_ = false;
  }

  // Low frame rate and variable frame rate content does not look smooth
  // with a cadence anyway: the pattern would change too often.
  if (is_variable_frame_rate_ || frame_duration > kMaxCadenceFrameDuration) {
    render_intervals_cadence_held_ = 0;
    if (!cadence_.empty()) {
      cadence_.clear();
      return true;
    }
    return false;
  }

  int64_t time_until_max_drift = 0;
  Cadence new_cadence = CalculateCadence(render_interval, frame_duration,
                                         max_acceptable_drift,
                                         &time_until_max_drift);

  // A new refresh rate (window moved to another screen) applies at once.
  if (render_interval_changed && new_cadence != cadence_) {
    LOGD("cadence %s -> %s", CadenceToString(cadence_).c_str(),
         CadenceToString(new_cadence).c_str());
    cadence_.swap(new_cadence);
    return true;
  }

  if (new_cadence == cadence_) {
    render_intervals_cadence_held_ = 0;
    return false;
  }

  bool update_pending_cadence = true;
  if (new_cadence == pending_cadence_ ||
      cadence_hysteresis_threshold_ <= render_interval) {
    if (++render_intervals_cadence_held_ * render_interval >=
        cadence_hysteresis_threshold_) {
      LOGD("cadence %s -> %s, %.1f s until max drift",
           CadenceToString(cadence_).c_str(),
           CadenceToString(new_cadence).c_str(), time_until_max_drift / 1e6);
      cadence_.swap(new_cadence);
      return true;
    }
    update_pending_cadence = false;
  }
  if (update_pending_cadence) {
    pending_cadence_.swap(new_cadence);
    render_intervals_cadence_held_ = 1;
  }
  return false;
}

int VideoCadenceEstimator::GetCadenceForFrame(uint64_t frame_number) const {
  return cadence_[frame_number % cadence_.size()];
}

VideoCadenceEstimator::Cadence VideoCadenceEstimator::CalculateCadence(
    int64_t render_interval,
    int64_t frame_duration,
    int64_t max_acceptable_drift,
    int64_t* time_until_max_drift) const {
  // Render intervals per frame.
  const double perfect_cadence =
      static_cast<double>(frame_duration) / render_interval;

  // Drift this large cannot build up within the minimum time anyway.
  if (max_acceptable_drift >= minimum_time_until_max_drift_) {
    return ConstructSimpleCadence(perfect_cadence, max_acceptable_drift,
                                  time_until_max_drift);
  }

  // Approximate the perfect cadence by k/n while keeping the drift rate low
  // enough.
  const double drift_ratio = static_cast<double>(max_acceptable_drift) /
                             minimum_time_until_max_drift_;
  const double minimum_acceptable_cadence =
      perfect_cadence / (1.0 + drift_ratio);
  const double maximum_acceptable_cadence =
      perfect_cadence / (1.0 - drift_ratio);

  double best_error = 0;
  int best_n = 0;
  int best_k = 0;
  for (int n = 1; n <= kMaxCadenceSize; ++n) {
    // A pattern of length n exists if some integer k makes k/n acceptable;
    // the best has the smallest error relative to the perfect cadence.
    if (std::floor(minimum_acceptable_cadence * n) <
        std::floor(maximum_acceptable_cadence * n)) {
      const int k = static_cast<int>(std::round(perfect_cadence * n));
      if (k <= 0) {
        continue;
      }
      const double error = std::fabs(1.0 - perfect_cadence * n / k);
      // Prefer the shorter pattern unless a longer one is clearly better.
      if (!best_n || error < best_error * 0.99) {
        best_error = error;
        best_k = k;
        best_n = n;
      }
    }
  }

  if (!best_n) {
    Cadence cadence = ConstructSimpleCadence(
        perfect_cadence, max_acceptable_drift, time_until_max_drift);
    if (!cadence.empty() &&
        *time_until_max_drift >= minimum_time_until_max_drift_) {
      return cadence;
    }
    return {};
  }

  *time_until_max_drift =
      best_error > 0 ? static_cast<int64_t>(max_acceptable_drift / best_error)
                     : kInfiniteTime;
  return ConstructCadence(best_k, best_n);
}

}  // namespace lychee
