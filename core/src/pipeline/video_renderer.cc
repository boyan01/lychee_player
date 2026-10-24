#define LOG_TAG "video_renderer"

#include "pipeline/video_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "base/log.h"
#include "base/thread_name.h"
#include "media/factory.h"

namespace lychee {

namespace {

// Decoded frames waiting to be shown, besides the one on screen.
constexpr size_t kMaxQueuedFrames = 5;
// Frames without a duration are assumed to last this long (30 fps).
constexpr int64_t kDefaultFrameDuration = kMicrosPerSecond / 30;
// Until the caller's display times show otherwise.
constexpr int64_t kDefaultRenderInterval = kMicrosPerSecond / 60;
// Display time deltas kept to estimate the vsync interval. Cadence
// detection needs the interval within about 0.1%; with timestamps jittered
// by a few milliseconds that takes seconds of samples.
constexpr size_t kRenderIntervalSamples = 600;
// This many deltas in a row far from the estimate mean a new refresh rate
// (the window moved to another screen): start over.
constexpr int kRefreshChangeSamples = 8;

int64_t FrameEnd(const VideoFrame& frame) {
  return frame.pts +
         (frame.duration > 0 ? frame.duration : kDefaultFrameDuration);
}

}  // namespace

VideoRenderer::VideoRenderer(MediaClock* clock, uint32_t accepted_storage)
    : clock_(clock),
      accepted_storage_(accepted_storage),
      converter_(CreateFrameConverter()),
      algorithm_([clock](const std::vector<int64_t>& media_times,
                         std::vector<int64_t>* system_times) {
        return clock->SystemTimesFor(media_times, system_times);
      }) {}

VideoRenderer::~VideoRenderer() {
  Stop();
}

bool VideoRenderer::Initialize(const VideoConfig& config,
                               bool allow_hardware,
                               PacketQueue* queue,
                               std::string* error) {
  queue_ = queue;
  for (auto& decoder : CreateVideoDecoders(allow_hardware)) {
    std::string decoder_error;
    if (decoder->Initialize(config, &decoder_error)) {
      decoder_ = std::move(decoder);
      return true;
    }
    LOGI("%s decoder rejected %s: %s", decoder->name(),
         config.codec_name.c_str(), decoder_error.c_str());
    *error = decoder_error;
  }
  if (error->empty()) {
    *error = "no video decoder in this build";
  }
  return false;
}

void VideoRenderer::Start() {
  thread_ = std::thread(&VideoRenderer::DecodeLoop, this);
}

void VideoRenderer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void VideoRenderer::DecodeLoop() {
  SetCurrentThreadName("lychee-vdecode");
  uint64_t decoder_generation = 0;
  PacketQueue::Entry entry;
  while (queue_->Pop(&entry)) {
    if (entry.generation != decoder_generation) {
      decoder_->Flush();
      decoder_generation = entry.generation;
    }
    uint64_t generation = entry.generation;
    bool ok = decoder_->Decode(
        *entry.packet, [this, generation](std::shared_ptr<VideoFrame> frame) {
          OnFrame(std::move(frame), generation);
        });
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ok) {
      LOGE("%s decoder failed", decoder_->name());
      failed_ = true;
      break;
    }
    if (entry.packet->end_of_stream && generation == generation_) {
      end_of_stream_ = true;
    }
    if (stopped_) {
      break;
    }
  }
}

void VideoRenderer::OnFrame(std::shared_ptr<VideoFrame> frame,
                            uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_) {
      return;
    }
    // Accurate seek: skip frames that end before the seek target.
    if (start_time_ != kNoTimestamp && frame->pts != kNoTimestamp &&
        FrameEnd(*frame) <= start_time_) {
      return;
    }
  }
  frame = Adapt(frame);
  if (!frame) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [&] {
    return stopped_ || generation != generation_ ||
           algorithm_.frames_queued() <= kMaxQueuedFrames;
  });
  if (stopped_ || generation != generation_) {
    return;
  }
  // Frame selection needs a timeline; continue the previous frame's.
  if (frame->pts == kNoTimestamp) {
    frame->pts = algorithm_.frames_queued()
                     ? FrameEnd(*algorithm_.last_frame())
                     : (start_time_ != kNoTimestamp ? start_time_ : 0);
  }
  algorithm_.EnqueueFrame(std::move(frame));
}

std::shared_ptr<VideoFrame> VideoRenderer::Adapt(
    const std::shared_ptr<VideoFrame>& frame) {
  std::shared_ptr<VideoFrame> result;
  if (converter_) {
    result = converter_->Convert(frame, accepted_storage_);
  } else if (accepted_storage_ & static_cast<uint32_t>(frame->storage)) {
    result = frame;
  }
  if (!result && !logged_unsupported_frame_) {
    logged_unsupported_frame_ = true;
    LOGE("cannot convert frame storage %u to accepted types 0x%x",
         static_cast<uint32_t>(frame->storage), accepted_storage_);
  }
  return result;
}

void VideoRenderer::Flush(uint64_t generation, int64_t start_time) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    start_time_ = start_time;
    algorithm_.Reset();
    selected_.reset();
    selected_vsync_ = kNoTimestamp;
    current_stale_ = true;
    end_of_stream_ = false;
  }
  condition_.notify_all();
}

bool VideoRenderer::HasEnoughData() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return algorithm_.effective_frames_queued() > 0 || end_of_stream_;
}

bool VideoRenderer::IsStarved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return algorithm_.effective_frames_queued() == 0 && !end_of_stream_;
}

bool VideoRenderer::shown_size(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shown_width_ <= 0 || shown_height_ <= 0) {
    return false;
  }
  *width = shown_width_;
  *height = shown_height_;
  return true;
}

bool VideoRenderer::HasError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

bool VideoRenderer::Ended(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!end_of_stream_ || algorithm_.frames_queued() > 1) {
    return false;
  }
  // The last frame has not been shown yet.
  if (algorithm_.frames_queued() == 1 &&
      algorithm_.front_frame() != current_) {
    return false;
  }
  if (!current_ || current_stale_) {
    return true;
  }
  return media_time >= FrameEnd(*current_);
}

std::shared_ptr<VideoFrame> VideoRenderer::SelectFrameLocked(
    int64_t display_time) {
  int64_t interval = RenderIntervalLocked(display_time);
  int64_t vsync = SnapToVsyncLocked(display_time, interval);
  // Already decided for this vsync: has_new_frame and acquire_frame are
  // usually called for the same one.
  if (selected_ && vsync == selected_vsync_) {
    return selected_;
  }
  if (algorithm_.frames_queued() == 0) {
    return current_;
  }
  size_t dropped = 0;
  selected_ = algorithm_.Render(vsync, vsync + interval, &dropped);
  selected_vsync_ = vsync;
  frames_dropped_ += static_cast<int64_t>(dropped);
  return selected_;
}

int64_t VideoRenderer::RenderIntervalLocked(int64_t display_time) {
  if (last_display_time_ != kNoTimestamp) {
    int64_t delta = display_time - last_display_time_;
    // Ignore pauses in pulling and repeated calls.
    if (delta > 2 * kMicrosPerMilli && delta < 100 * kMicrosPerMilli) {
      if (render_interval_ > 0 &&
          std::llabs(delta - render_interval_) > render_interval_ / 5) {
        if (++unexpected_intervals_ >= kRefreshChangeSamples) {
          display_intervals_.erase(
              display_intervals_.begin(),
              display_intervals_.end() - (kRefreshChangeSamples - 1));
          unexpected_intervals_ = 0;
        }
      } else {
        unexpected_intervals_ = 0;
      }
      display_intervals_.push_back(delta);
      if (display_intervals_.size() > kRenderIntervalSamples) {
        display_intervals_.pop_front();
      }
    }
  }
  last_display_time_ = display_time;
  if (display_intervals_.empty()) {
    return kDefaultRenderInterval;
  }
  render_interval_ = EstimateRenderInterval(display_intervals_);
  return render_interval_;
}

int64_t VideoRenderer::EstimateRenderInterval(
    const std::deque<int64_t>& display_intervals) {
  // Time spanned divided by vsyncs spanned: timestamp jitter cancels out
  // between consecutive deltas, except at the two ends. The median tells
  // how many vsyncs a delta spans when the caller skipped some.
  std::vector<int64_t> sorted(display_intervals.begin(),
                              display_intervals.end());
  auto middle = sorted.begin() + sorted.size() / 2;
  std::nth_element(sorted.begin(), middle, sorted.end());
  const double median = static_cast<double>(*middle);
  int64_t span = 0;
  int64_t vsyncs = 0;
  for (int64_t interval : display_intervals) {
    span += interval;
    vsyncs += std::max<int64_t>(std::llround(interval / median), 1);
  }
  return span / vsyncs;
}

int64_t VideoRenderer::SnapToVsyncLocked(int64_t display_time,
                                         int64_t interval) {
  if (vsync_grid_ != kNoTimestamp) {
    int64_t vsyncs = static_cast<int64_t>(
        std::llround(static_cast<double>(display_time - vsync_grid_) /
                     static_cast<double>(interval)));
    int64_t predicted = vsync_grid_ + vsyncs * interval;
    int64_t error = display_time - predicted;
    if (vsyncs >= 0 && std::llabs(error) < interval / 3) {
      if (vsyncs == 0) {
        return vsync_grid_;  // The same vsync again.
      }
      // Follow slow drift between the estimate and the real refresh rate.
      vsync_grid_ = predicted + error / 16;
      return vsync_grid_;
    }
  }
  vsync_grid_ = display_time;
  return display_time;
}

bool VideoRenderer::HasNewFrame(int64_t display_time) {
  bool result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<VideoFrame> frame = SelectFrameLocked(display_time);
    result = frame && frame != current_;
  }
  // Selection may have freed queue space.
  condition_.notify_all();
  return result;
}

std::shared_ptr<VideoFrame> VideoRenderer::Acquire(int64_t display_time) {
  std::shared_ptr<VideoFrame> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::shared_ptr<VideoFrame> frame = SelectFrameLocked(display_time);
    if (frame && frame != current_) {
      frames_rendered_++;
      current_ = std::move(frame);
      current_stale_ = false;
      shown_width_ = current_->width;
      shown_height_ = current_->height;
    }
    result = current_;
  }
  condition_.notify_all();
  return result;
}

const char* VideoRenderer::decoder_name() const {
  return decoder_ ? decoder_->name() : "none";
}

bool VideoRenderer::is_hardware() const {
  return decoder_ && decoder_->is_hardware();
}

int64_t VideoRenderer::frames_rendered() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_rendered_;
}

int64_t VideoRenderer::frames_dropped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_dropped_;
}

}  // namespace lychee
