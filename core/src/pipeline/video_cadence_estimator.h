// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from Chromium's media/filters/video_cadence_estimator.h, with
// microsecond integers in place of base::TimeDelta and without the UMA and
// testing hooks.

#ifndef LYCHEE_PIPELINE_VIDEO_CADENCE_ESTIMATOR_H_
#define LYCHEE_PIPELINE_VIDEO_CADENCE_ESTIMATOR_H_

#include <cstdint>
#include <vector>

namespace lychee {

// Estimates whether a frame duration and a render (vsync) interval have a
// cadence: a repeating pattern of how many intervals each frame stays on
// screen that keeps displayed durations uniform.
//
// The perfect cadence is frame duration / render interval: 2 for 30 fps at
// 60 Hz, ~2.0029 for 29.97 fps at 60 Hz, 2.5 for 24 fps at 60 Hz. A cadence
// [a1:..:aN] (N <= 5) of integers approximates it: [2] for 29.97 fps, [2:3]
// for 24 fps. Rounding makes frame timestamps drift from their display time,
// so a cadence is only used if reaching the maximum acceptable drift takes
// at least |minimum_time_until_max_drift|; then a frame is dropped or
// repeated that rarely, which is less visible than irregular durations.
class VideoCadenceEstimator {
 public:
  using Cadence = std::vector<int>;

  explicit VideoCadenceEstimator(int64_t minimum_time_until_max_drift);

  void Reset();

  // Recomputes the cadence. Returns true if it changed; callers then update
  // their frames from GetCadenceForFrame(). All arguments in microseconds.
  bool UpdateCadenceEstimate(int64_t render_interval,
                             int64_t frame_duration,
                             int64_t frame_duration_deviation,
                             int64_t max_acceptable_drift);

  bool has_cadence() const { return !cadence_.empty(); }

  // Render intervals the frame |frame_number| should be shown for, counting
  // from the frame where the current cadence started. 0 means skip it.
  int GetCadenceForFrame(uint64_t frame_number) const;

 private:
  Cadence CalculateCadence(int64_t render_interval,
                           int64_t frame_duration,
                           int64_t max_acceptable_drift,
                           int64_t* time_until_max_drift) const;

  Cadence cadence_;

  // A new cadence must persist for a while before it replaces the current
  // one, so the estimate does not oscillate.
  Cadence pending_cadence_;
  int render_intervals_cadence_held_ = 0;
  const int64_t cadence_hysteresis_threshold_;

  const int64_t minimum_time_until_max_drift_;

  bool is_variable_frame_rate_ = false;
  int64_t last_render_interval_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_VIDEO_CADENCE_ESTIMATOR_H_
