// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from Chromium's media/filters/video_renderer_algorithm.h, with
// microsecond integers in place of base::TimeTicks/TimeDelta, and without
// memory accounting, OnLastFrameDropped() and the testing hooks.

#ifndef LYCHEE_PIPELINE_VIDEO_RENDERER_ALGORITHM_H_
#define LYCHEE_PIPELINE_VIDEO_RENDERER_ALGORITHM_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "media/video_frame.h"
#include "pipeline/video_cadence_estimator.h"

namespace lychee {

// Chooses which queued frame to show for each render (vsync) interval, so
// displayed frame durations stay as uniform as possible.
//
// The primary means is a forced integer cadence (see VideoCadenceEstimator).
// Without one, it picks the frame that covers most of the interval; if no
// frame covers it, the frame that drifts least from its start.
//
// Render() must be called once per interval for the cadence to hold.
class VideoRendererAlgorithm {
 public:
  // Converts media timestamps to wall clock (display timebase) times.
  // Returns false if the media clock is not running; the times are then
  // estimates.
  using WallClockTimeCB = std::function<bool(const std::vector<int64_t>& media,
                                             std::vector<int64_t>* wall)>;

  explicit VideoRendererAlgorithm(WallClockTimeCB wall_clock_time_cb);

  // The best frame for the interval [deadline_min, deadline_max], or null if
  // no frame is queued. |frames_dropped| receives the number of frames
  // removed without ever being returned.
  std::shared_ptr<VideoFrame> Render(int64_t deadline_min,
                                     int64_t deadline_max,
                                     size_t* frames_dropped);

  // Removes frames too old to show at |deadline|, keeping at least one.
  // Returns how many were never rendered.
  size_t RemoveExpiredFrames(int64_t deadline);

  // Not in Chromium: removes the queued frames at or after |pts|, except
  // the front one.
  void RemoveFramesFrom(int64_t pts);

  // Adds a decoded frame; out-of-order frames are sorted in. Frames at or
  // before the last rendered one, or within 1 ms of a queued one, are
  // dropped (and reported by the next Render()).
  void EnqueueFrame(std::shared_ptr<VideoFrame> frame);

  // Empties the queue. kPreserveNextFrameEstimates keeps the frame duration
  // estimate, so a seek does not restart cadence detection from scratch.
  enum class ResetFlag { kEverything, kPreserveNextFrameEstimates };
  void Reset(ResetFlag reset_flag = ResetFlag::kEverything);

  // Frames that could still be rendered given the current render intervals:
  // frames with a nonzero ideal render count under a cadence, otherwise
  // frames ending after the last interval.
  size_t effective_frames_queued() const { return effective_frames_queued_; }
  size_t frames_queued() const { return frame_queue_.size(); }
  // The frame Render() returned last, or the next one if it never ran.
  const std::shared_ptr<VideoFrame>& front_frame() const {
    return frame_queue_.front().frame;
  }
  const std::shared_ptr<VideoFrame>& last_frame() const {
    return frame_queue_.back().frame;
  }

 private:
  // Seconds before a cadence's drift forces a dropped or repeated frame
  // that make it acceptable. 8 s allows 29.97 fps and 59.94 fps at 60 Hz and
  // vice versa; few viewers notice one glitch every 8 s.
  static constexpr int64_t kMinimumAcceptableTimeBetweenGlitches = 8000000;
  // Frames in the frame duration moving average.
  static constexpr size_t kMovingAverageSamples = 32;

  struct ReadyFrame {
    explicit ReadyFrame(std::shared_ptr<VideoFrame> frame)
        : frame(std::move(frame)) {}

    std::shared_ptr<VideoFrame> frame;
    // Wall clock times. |start_time| is valid after UpdateFrameStatistics(),
    // |end_time| once a later frame is queued (or estimated before).
    int64_t start_time = 0;
    int64_t end_time = 0;
    bool has_estimated_end_time = true;
    int ideal_render_count = 0;
    int render_count = 0;
    int drop_count = 0;
  };

  // Mean and standard deviation of the last few samples.
  class MovingAverage {
   public:
    explicit MovingAverage(size_t depth) : depth_(depth) {}
    void AddSample(int64_t sample);
    size_t Count() const { return samples_.size(); }
    int64_t Mean() const;
    int64_t Deviation() const;
    void Reset();

   private:
    const size_t depth_;
    std::deque<int64_t> samples_;
    int64_t sum_ = 0;
    double sum_of_squares_ = 0;
  };

  // Counts intervals skipped since the last Render() as renders of the
  // frame that stayed on screen.
  void AccountForMissedIntervals(int64_t deadline_min, int64_t deadline_max);
  // Refreshes the wall clock times of all frames, the frame duration
  // estimate, the maximum acceptable drift and the cadence.
  void UpdateFrameStatistics();
  void UpdateCadenceForFrames();
  int FindBestFrameByCadence() const;
  int FindBestFrameByCoverage(int64_t deadline_min,
                              int64_t deadline_max,
                              int* second_best) const;
  int FindBestFrameByDrift(int64_t deadline_min,
                           int64_t* selected_frame_drift) const;
  int64_t CalculateAbsoluteDriftForFrame(int64_t deadline_min,
                                         int frame_index) const;
  int FindFirstGoodFrame() const;
  void UpdateEffectiveFramesQueued();

  std::deque<ReadyFrame> frame_queue_;
  VideoCadenceEstimator cadence_estimator_;
  bool have_rendered_frames_ = false;
  const WallClockTimeCB wall_clock_time_cb_;
  // 0 until the first Render().
  int64_t last_deadline_max_ = 0;
  MovingAverage frame_duration_calculator_;
  int64_t average_frame_duration_ = 0;
  int64_t render_interval_ = 0;
  int64_t max_acceptable_drift_ = 0;
  size_t frames_dropped_during_enqueue_ = 0;
  // Under a cadence, do not count against it until the first frame reached
  // its presentation time.
  bool first_frame_ = true;
  // Frame number of the front frame, counted from where the cadence began.
  uint64_t cadence_frame_counter_ = 0;
  bool was_time_moving_ = false;
  size_t effective_frames_queued_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_VIDEO_RENDERER_ALGORITHM_H_
