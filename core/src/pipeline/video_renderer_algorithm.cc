// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Ported from Chromium's media/filters/video_renderer_algorithm.cc.

#include "pipeline/video_renderer_algorithm.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include "base/time.h"

namespace lychee {

namespace {

constexpr int64_t kMaxTime = std::numeric_limits<int64_t>::max();

}  // namespace

void VideoRendererAlgorithm::MovingAverage::AddSample(int64_t sample) {
  samples_.push_back(sample);
  sum_ += sample;
  sum_of_squares_ += static_cast<double>(sample) * sample;
  if (samples_.size() > depth_) {
    int64_t oldest = samples_.front();
    samples_.pop_front();
    sum_ -= oldest;
    sum_of_squares_ -= static_cast<double>(oldest) * oldest;
  }
}

int64_t VideoRendererAlgorithm::MovingAverage::Mean() const {
  return samples_.empty() ? 0 : sum_ / static_cast<int64_t>(samples_.size());
}

int64_t VideoRendererAlgorithm::MovingAverage::Deviation() const {
  if (samples_.empty()) {
    return 0;
  }
  const double n = static_cast<double>(samples_.size());
  const double mean = sum_ / n;
  const double variance = std::max(sum_of_squares_ / n - mean * mean, 0.0);
  return static_cast<int64_t>(std::sqrt(variance));
}

void VideoRendererAlgorithm::MovingAverage::Reset() {
  samples_.clear();
  sum_ = 0;
  sum_of_squares_ = 0;
}

VideoRendererAlgorithm::VideoRendererAlgorithm(
    WallClockTimeCB wall_clock_time_cb)
    : cadence_estimator_(kMinimumAcceptableTimeBetweenGlitches),
      wall_clock_time_cb_(std::move(wall_clock_time_cb)),
      frame_duration_calculator_(kMovingAverageSamples) {
  Reset();
}

std::shared_ptr<VideoFrame> VideoRendererAlgorithm::Render(
    int64_t deadline_min,
    int64_t deadline_max,
    size_t* frames_dropped) {
  if (frame_queue_.empty()) {
    return nullptr;
  }
  if (frames_dropped) {
    *frames_dropped = frames_dropped_during_enqueue_;
  }
  frames_dropped_during_enqueue_ = 0;
  have_rendered_frames_ = true;

  // Step 1: the current render interval, for the subroutines.
  render_interval_ = deadline_max - deadline_min;

  // Step 2: intervals skipped since the last call showed the front frame.
  AccountForMissedIntervals(deadline_min, deadline_max);

  // Step 3: wall clock times and frame duration estimates.
  UpdateFrameStatistics();
  if (!was_time_moving_ || average_frame_duration_ <= 0 ||
      render_interval_ == 0) {
    ReadyFrame& ready_frame = frame_queue_.front();
    ++ready_frame.render_count;
    UpdateEffectiveFramesQueued();
    if (!was_time_moving_) {
      first_frame_ = true;
    }
    return ready_frame.frame;
  }

  last_deadline_max_ = deadline_max;
  int64_t selected_frame_drift = 0;

  // Step 4: the best frame by cadence.
  const int cadence_frame = FindBestFrameByCadence();
  int frame_to_render = cadence_frame;
  if (frame_to_render >= 0) {
    selected_frame_drift =
        CalculateAbsoluteDriftForFrame(deadline_min, frame_to_render);
  }

  // Step 5: no cadence frame, or it drifted too far: the frame covering most
  // of the interval.
  if (frame_to_render < 0 || selected_frame_drift > max_acceptable_drift_) {
    int second_best_by_coverage = -1;
    const int best_by_coverage = FindBestFrameByCoverage(
        deadline_min, deadline_max, &second_best_by_coverage);
    // Here only because the cadence frame drifted too far: even if it covers
    // most, prefer the runner-up if that drifts less.
    if (frame_to_render == best_by_coverage && second_best_by_coverage >= 0 &&
        CalculateAbsoluteDriftForFrame(deadline_min, second_best_by_coverage) <=
            selected_frame_drift) {
      frame_to_render = second_best_by_coverage;
    } else {
      frame_to_render = best_by_coverage;
    }
    if (frame_to_render >= 0) {
      selected_frame_drift =
          CalculateAbsoluteDriftForFrame(deadline_min, frame_to_render);
    }
  }

  // Step 6: still nothing (no frame covers the interval): the least drift.
  if (frame_to_render < 0 || selected_frame_drift > max_acceptable_drift_) {
    frame_to_render = FindBestFrameByDrift(deadline_min, &selected_frame_drift);
  }

  const bool ignored_cadence_frame =
      cadence_frame >= 0 && frame_to_render != cadence_frame;

  // Step 7: drop frames before the chosen one. Frames a cadence meant to
  // skip, and frames shown at least once, do not count as dropped.
  if (frame_to_render > 0) {
    if (frames_dropped) {
      for (int i = 0; i < frame_to_render; ++i) {
        const ReadyFrame& frame = frame_queue_[i];
        if (frame.render_count != frame.drop_count) {
          continue;
        }
        if (cadence_estimator_.has_cadence() && !frame.ideal_render_count) {
          continue;
        }
        ++(*frames_dropped);
      }
    }
    cadence_frame_counter_ += frame_to_render;
    frame_queue_.erase(frame_queue_.begin(),
                       frame_queue_.begin() + frame_to_render);
  }

  // Step 8: the frame is chosen.
  if (first_frame_ && frame_to_render > 0) {
    first_frame_ = false;
  }
  ++frame_queue_.front().render_count;

  // A broken cadence sequence restarts from the chosen frame.
  if (ignored_cadence_frame) {
    cadence_frame_counter_ = 0;
    UpdateCadenceForFrames();
  }

  UpdateEffectiveFramesQueued();
  return frame_queue_.front().frame;
}

size_t VideoRendererAlgorithm::RemoveExpiredFrames(int64_t deadline) {
  // Keep this current, or the effective frame count never expires the last
  // frame.
  if (deadline > last_deadline_max_) {
    last_deadline_max_ = deadline;
  }
  if (frame_queue_.empty()) {
    return 0;
  }
  UpdateFrameStatistics();
  UpdateEffectiveFramesQueued();
  if (frame_queue_.size() == 1 || average_frame_duration_ <= 0) {
    return 0;
  }

  const int first_good_frame = FindFirstGoodFrame();
  const size_t frames_to_expire =
      first_good_frame < 0 ? frame_queue_.size() - 1 : first_good_frame;
  if (!frames_to_expire) {
    return 0;
  }

  size_t frames_dropped_without_rendering = 0;
  for (size_t i = 0; i < frames_to_expire; ++i) {
    const ReadyFrame& frame = frame_queue_[i];
    if (frame.render_count == frame.drop_count &&
        (!cadence_estimator_.has_cadence() || frame.ideal_render_count)) {
      ++frames_dropped_without_rendering;
    }
  }
  cadence_frame_counter_ += frames_to_expire;
  frame_queue_.erase(frame_queue_.begin(),
                     frame_queue_.begin() + frames_to_expire);
  UpdateEffectiveFramesQueued();
  return frames_dropped_without_rendering;
}

void VideoRendererAlgorithm::RemoveFramesFrom(int64_t pts) {
  while (frame_queue_.size() > 1 && frame_queue_.back().frame->pts >= pts) {
    frame_queue_.pop_back();
  }
  UpdateEffectiveFramesQueued();
}

void VideoRendererAlgorithm::Reset(ResetFlag reset_flag) {
  frames_dropped_during_enqueue_ = 0;
  have_rendered_frames_ = false;
  render_interval_ = 0;
  frame_queue_.clear();
  cadence_estimator_.Reset();
  if (reset_flag != ResetFlag::kPreserveNextFrameEstimates) {
    average_frame_duration_ = 0;
    last_deadline_max_ = 0;
    frame_duration_calculator_.Reset();
  }
  first_frame_ = true;
  effective_frames_queued_ = 0;
  cadence_frame_counter_ = 0;
  was_time_moving_ = false;
  // ATSC IS/191's maximum acceptable drift, until frame durations are known.
  max_acceptable_drift_ = 15 * kMicrosPerMilli;
}

void VideoRendererAlgorithm::EnqueueFrame(std::shared_ptr<VideoFrame> frame) {
  const int64_t timestamp = frame->pts;
  const int64_t metadata_frame_duration = frame->duration;
  ReadyFrame ready_frame(std::move(frame));
  auto it = std::lower_bound(
      frame_queue_.begin(), frame_queue_.end(), timestamp,
      [](const ReadyFrame& queued, int64_t t) { return queued.frame->pts < t; });
  const size_t new_frame_index = it - frame_queue_.begin();

  // Too late: at or before the frame already rendered.
  if (new_frame_index == 0 && have_rendered_frames_) {
    ++frames_dropped_during_enqueue_;
    return;
  }

  // Frames under a millisecond apart (including equal timestamps) cannot be
  // shown; effectively a 1000 fps limit.
  const int64_t delta = std::min(
      new_frame_index < frame_queue_.size()
          ? frame_queue_[new_frame_index].frame->pts - timestamp
          : kMaxTime,
      new_frame_index > 0
          ? timestamp - frame_queue_[new_frame_index - 1].frame->pts
          : kMaxTime);
  if (delta < kMicrosPerMilli) {
    ++frames_dropped_during_enqueue_;
    return;
  }

  // Start time now and an estimated end time, so the effective frame count
  // is roughly right before the next Render().
  std::vector<int64_t> media_timestamps(1, timestamp);
  if (!frame_duration_calculator_.Count() && metadata_frame_duration > 0) {
    media_timestamps.push_back(timestamp + metadata_frame_duration);
  }
  std::vector<int64_t> wall_clock_times;
  wall_clock_time_cb_(media_timestamps, &wall_clock_times);
  ready_frame.start_time = wall_clock_times[0];
  if (frame_duration_calculator_.Count()) {
    ready_frame.end_time = ready_frame.start_time + average_frame_duration_;
  } else if (wall_clock_times.size() > 1) {
    ready_frame.end_time = wall_clock_times[1];
  }

  frame_queue_.insert(it, std::move(ready_frame));
  if (cadence_estimator_.has_cadence()) {
    UpdateCadenceForFrames();
  }
  UpdateEffectiveFramesQueued();
}

void VideoRendererAlgorithm::AccountForMissedIntervals(int64_t deadline_min,
                                                       int64_t deadline_max) {
  if (last_deadline_max_ == 0 || deadline_min <= last_deadline_max_ ||
      !have_rendered_frames_ || !was_time_moving_ || render_interval_ == 0) {
    return;
  }
  const int64_t render_cycle_count =
      (deadline_min - last_deadline_max_) / render_interval_;
  if (!render_cycle_count) {
    return;
  }
  // Only if the front frame was rendered at all: it may not have been yet,
  // or earlier frames were expired.
  ReadyFrame& ready_frame = frame_queue_.front();
  if (!ready_frame.render_count) {
    return;
  }
  // A frame dropped on every attempt keeps counting as dropped.
  if (ready_frame.render_count == ready_frame.drop_count) {
    ready_frame.drop_count += static_cast<int>(render_cycle_count);
  }
  ready_frame.render_count += static_cast<int>(render_cycle_count);
}

void VideoRendererAlgorithm::UpdateFrameStatistics() {
  std::vector<int64_t> media_timestamps;
  media_timestamps.reserve(frame_queue_.size() + 1);
  for (const auto& ready_frame : frame_queue_) {
    media_timestamps.push_back(ready_frame.frame->pts);
  }
  // The last frame's own duration estimates its end, which matters when
  // playback ends on a long frame.
  const VideoFrame& last = *frame_queue_.back().frame;
  const bool have_metadata_duration = last.duration > 0;
  if (have_metadata_duration) {
    media_timestamps.push_back(last.pts + last.duration);
  }

  std::vector<int64_t> wall_clock_times;
  was_time_moving_ = wall_clock_time_cb_(media_timestamps, &wall_clock_times);

  // All frames but the last: their end is the next frame's start. A frame
  // gets a real end time once, which makes it a new duration sample.
  for (size_t i = 0; i + 1 < frame_queue_.size(); ++i) {
    ReadyFrame& frame = frame_queue_[i];
    const bool new_sample = frame.has_estimated_end_time;
    frame.start_time = wall_clock_times[i];
    frame.end_time = wall_clock_times[i + 1];
    frame.has_estimated_end_time = false;
    if (new_sample) {
      frame_duration_calculator_.AddSample(frame.end_time - frame.start_time);
    }
  }

  int64_t deviation = 0;
  if (frame_duration_calculator_.Count()) {
    average_frame_duration_ = frame_duration_calculator_.Mean();
    deviation = frame_duration_calculator_.Deviation();
  }

  ReadyFrame& back = frame_queue_.back();
  if (have_metadata_duration) {
    back.start_time = wall_clock_times.end()[-2];
    back.end_time = wall_clock_times.end()[-1];
    // Measured durations reflect time on screen better; use the metadata
    // only without samples.
    if (!frame_duration_calculator_.Count()) {
      average_frame_duration_ = back.end_time - back.start_time;
      if (average_frame_duration_ <= 0) {
        return;
      }
    }
  } else {
    back.start_time = wall_clock_times.back();
    // The cadence needs a duration and its deviation.
    if (!frame_duration_calculator_.Count()) {
      return;
    }
    back.end_time = back.start_time + average_frame_duration_;
  }

  // ITU-R BR.265 recommends at most half a frame of drift. Allow at least
  // 1/60 s (below the threshold of detection, and stable for 60 fps) and at
  // most 1/8 s (beyond that low frame rates show visible differences).
  max_acceptable_drift_ =
      std::clamp<int64_t>(average_frame_duration_ / 2, kMicrosPerSecond / 60,
                          kMicrosPerSecond / 8);

  // Called from RemoveExpiredFrames() before any Render().
  if (render_interval_ == 0) {
    return;
  }
  if (cadence_estimator_.UpdateCadenceEstimate(
          render_interval_, average_frame_duration_, deviation,
          max_acceptable_drift_)) {
    cadence_frame_counter_ = 0;
    UpdateCadenceForFrames();
  }
}

void VideoRendererAlgorithm::UpdateCadenceForFrames() {
  for (size_t i = 0; i < frame_queue_.size(); ++i) {
    frame_queue_[i].ideal_render_count =
        cadence_estimator_.has_cadence()
            ? cadence_estimator_.GetCadenceForFrame(cadence_frame_counter_ + i)
            : 0;
  }
}

int VideoRendererAlgorithm::FindBestFrameByCadence() const {
  if (!cadence_estimator_.has_cadence()) {
    return -1;
  }
  // The current frame is below its cadence: keep it.
  const ReadyFrame& current_frame = frame_queue_.front();
  if (current_frame.render_count < current_frame.ideal_render_count) {
    return 0;
  }
  // Otherwise the next frame the cadence shows.
  for (size_t i = 1; i < frame_queue_.size(); ++i) {
    if (frame_queue_[i].ideal_render_count > 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int VideoRendererAlgorithm::FindBestFrameByCoverage(int64_t deadline_min,
                                                    int64_t deadline_max,
                                                    int* second_best) const {
  int best_frame_by_coverage = -1;
  int64_t best_coverage = 0;
  std::vector<int64_t> coverage(frame_queue_.size(), 0);
  for (size_t i = 0; i < frame_queue_.size(); ++i) {
    const ReadyFrame& frame = frame_queue_[i];
    if (frame.start_time > deadline_max) {
      break;
    }
    const int64_t end_time = std::min(deadline_max, frame.end_time);
    if (end_time < deadline_min) {
      continue;
    }
    coverage[i] = end_time - std::max(deadline_min, frame.start_time);
    if (coverage[i] > best_coverage) {
      best_frame_by_coverage = static_cast<int>(i);
      best_coverage = coverage[i];
    }
  }

  *second_best = -1;
  if (best_frame_by_coverage >= 0) {
    coverage[best_frame_by_coverage] = 0;
    auto it = std::max_element(coverage.begin(), coverage.end());
    if (*it > 0) {
      *second_best = static_cast<int>(it - coverage.begin());
    }
  }

  // Within half a millisecond, prefer the earlier frame. Chosen so 24 fps at
  // 60 Hz with +/-100 us of interval jitter selects consistently (3% at
  // 60 Hz).
  constexpr int64_t kAllowableJitter = 500;
  if (*second_best >= 0 && best_frame_by_coverage > *second_best &&
      std::llabs(best_coverage - coverage[*second_best]) <= kAllowableJitter) {
    std::swap(best_frame_by_coverage, *second_best);
  }
  return best_frame_by_coverage;
}

int VideoRendererAlgorithm::FindBestFrameByDrift(
    int64_t deadline_min,
    int64_t* selected_frame_drift) const {
  int best_frame_by_drift = -1;
  *selected_frame_drift = kMaxTime;
  for (size_t i = 0; i < frame_queue_.size(); ++i) {
    const int64_t drift =
        CalculateAbsoluteDriftForFrame(deadline_min, static_cast<int>(i));
    // <= prefers the latest frame with the least drift.
    if (drift <= *selected_frame_drift) {
      *selected_frame_drift = drift;
      best_frame_by_drift = static_cast<int>(i);
    }
  }
  return best_frame_by_drift;
}

int64_t VideoRendererAlgorithm::CalculateAbsoluteDriftForFrame(
    int64_t deadline_min,
    int frame_index) const {
  const ReadyFrame& frame = frame_queue_[frame_index];
  if (frame.end_time < deadline_min) {
    return deadline_min - frame.end_time;
  }
  if (frame.start_time > deadline_min) {
    return frame.start_time - deadline_min;
  }
  return 0;
}

int VideoRendererAlgorithm::FindFirstGoodFrame() const {
  const int64_t minimum_start_time =
      cadence_estimator_.has_cadence()
          ? last_deadline_max_ - max_acceptable_drift_
          : last_deadline_max_;
  size_t start_index = 0;
  for (; start_index < frame_queue_.size(); ++start_index) {
    const ReadyFrame& frame = frame_queue_[start_index];
    if ((!cadence_estimator_.has_cadence() ||
         frame.render_count < frame.ideal_render_count) &&
        (frame.end_time == 0 || frame.end_time > minimum_start_time)) {
      break;
    }
  }
  return start_index == frame_queue_.size() ? -1
                                            : static_cast<int>(start_index);
}

void VideoRendererAlgorithm::UpdateEffectiveFramesQueued() {
  if (frame_queue_.empty() || average_frame_duration_ <= 0 ||
      last_deadline_max_ == 0) {
    effective_frames_queued_ = frame_queue_.size();
    return;
  }
  const int start_index = FindFirstGoodFrame();
  if (start_index < 0) {
    effective_frames_queued_ = 0;
    return;
  }
  if (!cadence_estimator_.has_cadence()) {
    effective_frames_queued_ = frame_queue_.size() - start_index;
    return;
  }
  // Frames the cadence skips do not count.
  size_t renderable_frame_count = 0;
  for (size_t i = start_index; i < frame_queue_.size(); ++i) {
    if (frame_queue_[i].ideal_render_count) {
      ++renderable_frame_count;
    }
  }
  effective_frames_queued_ = renderable_frame_count;
}

}  // namespace lychee
