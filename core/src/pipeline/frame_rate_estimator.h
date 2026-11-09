#ifndef LYCHEE_PIPELINE_FRAME_RATE_ESTIMATOR_H_
#define LYCHEE_PIPELINE_FRAME_RATE_ESTIMATOR_H_

#include <cstdint>

#include "base/time.h"

namespace lychee {

// The frame rate of video whose container does not say it (MPEG-TS, FLV,
// AVI), from the display times of the frames a decoder outputs: once
// kFramesToSync frame durations in a row each lie within kTolerance of
// their mean, frames per second over that run, snapped to the standard
// rate it is within 0.05% of (24000/1001, 25, 30000/1001, ...).
// Timestamps in milliseconds (FLV) alternate between 41 and 42 ms at
// 23.976 fps, which the tolerance allows. Variable frame rate video, whose
// durations never settle, has none. The same idea as ExoPlayer's
// FixedFrameRateEstimator, written from its description.
class FrameRateEstimator {
 public:
  static constexpr int kFramesToSync = 15;
  static constexpr int64_t kTolerance = kMicrosPerMilli;

  // The next frame's display time, in presentation order.
  void AddFrame(int64_t pts);
  // A discontinuity (a seek): the next frame starts a new run. The last
  // estimate stands until a new run syncs.
  void Restart();
  // Frames per second, or 0 if unknown.
  double frame_rate() const { return frame_rate_; }

 private:
  int64_t last_pts_ = kNoTimestamp;
  // The run of matching durations: its first frame and its length.
  int64_t run_start_ = kNoTimestamp;
  int run_frames_ = 0;
  // Durations since the last synced run ended; that many without a new
  // one is variable frame rate.
  int unsynced_ = 0;
  double frame_rate_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_FRAME_RATE_ESTIMATOR_H_
