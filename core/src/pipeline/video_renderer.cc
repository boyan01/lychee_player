#define LOG_TAG "video_renderer"

#include "pipeline/video_renderer.h"

#include <algorithm>

#include "base/log.h"
#include "base/thread_name.h"
#include "media/factory.h"

namespace lychee {

namespace {

constexpr size_t kMaxQueuedFrames = 5;
// Frames without a duration are assumed to last this long (30 fps).
constexpr int64_t kDefaultFrameDuration = kMicrosPerSecond / 30;

int64_t FrameEnd(const VideoFrame& frame) {
  return frame.pts +
         (frame.duration > 0 ? frame.duration : kDefaultFrameDuration);
}

}  // namespace

VideoRenderer::VideoRenderer(MediaClock* clock, uint32_t accepted_storage)
    : clock_(clock),
      accepted_storage_(accepted_storage),
      converter_(CreateFrameConverter()) {}

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
    if (!ok) {
      LOGE("%s decoder failed", decoder_->name());
    }
    if (entry.packet->end_of_stream || !ok) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation == generation_) {
        end_of_stream_ = true;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
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
           frames_.size() < kMaxQueuedFrames;
  });
  if (stopped_ || generation != generation_) {
    return;
  }
  frames_.push_back(std::move(frame));
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
    frames_.clear();
    current_stale_ = true;
    end_of_stream_ = false;
  }
  condition_.notify_all();
}

bool VideoRenderer::HasEnoughData() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !frames_.empty() || end_of_stream_;
}

bool VideoRenderer::IsStarved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_.empty() && !end_of_stream_;
}

bool VideoRenderer::Ended(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!end_of_stream_ || !frames_.empty()) {
    return false;
  }
  if (!current_ || current_stale_ || current_->pts == kNoTimestamp) {
    return true;
  }
  return media_time >= FrameEnd(*current_);
}

int VideoRenderer::SelectFrameLocked(int64_t display_time) const {
  if (frames_.empty()) {
    return -1;
  }
  if (!current_ || current_stale_) {
    // Show the first frame after open or seek right away, even if paused.
    return 0;
  }
  int64_t media_time = clock_->MediaTimeAt(display_time);
  int selected = -1;
  for (size_t i = 0; i < frames_.size(); ++i) {
    const VideoFrame& frame = *frames_[i];
    // A frame is due once its pts is reached; half a frame of tolerance
    // rounds to the nearest vsync.
    int64_t tolerance =
        std::clamp<int64_t>(frame.duration / 2, 0, 20 * kMicrosPerMilli);
    if (frame.pts == kNoTimestamp || frame.pts <= media_time + tolerance) {
      selected = static_cast<int>(i);
    } else {
      break;
    }
  }
  return selected;
}

bool VideoRenderer::HasNewFrame(int64_t display_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SelectFrameLocked(display_time) >= 0;
}

std::shared_ptr<VideoFrame> VideoRenderer::Acquire(int64_t display_time) {
  std::shared_ptr<VideoFrame> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    int selected = SelectFrameLocked(display_time);
    if (selected >= 0) {
      frames_dropped_ += selected;
      frames_rendered_++;
      current_ = frames_[selected];
      current_stale_ = false;
      frames_.erase(frames_.begin(), frames_.begin() + selected + 1);
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
