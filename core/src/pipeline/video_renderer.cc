#define LOG_TAG "video_renderer"

#include "pipeline/video_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "base/log.h"
#include "base/time.h"
#include "base/thread_name.h"
#include "media/factory.h"

namespace lychee {

namespace {

// Decoded frames waiting to be shown, besides the one on screen.
constexpr size_t kMaxQueuedFrames = 5;
// No pull for this long counts as hidden (ExpireFramesWhileHidden()).
constexpr int64_t kHiddenTimeout = 250 * kMicrosPerMilli;
// Frames without a duration are assumed to last this long (30 fps).
constexpr int64_t kDefaultFrameDuration = kMicrosPerSecond / 30;
int64_t FrameEnd(const VideoFrame& frame) {
  return frame.pts +
         (frame.duration > 0 ? frame.duration : kDefaultFrameDuration);
}

}  // namespace

VideoRenderer::VideoRenderer(MediaClock* clock,
                             uint32_t accepted_storage,
                             bool rotate_surface_frames)
    : clock_(clock),
      accepted_storage_(accepted_storage),
      rotate_surface_frames_(rotate_surface_frames),
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
                               const VideoDevice& device,
                               const std::shared_ptr<void>& surface,
                               PacketQueue* queue,
                               std::string* error) {
  queue_ = queue;
  config_ = config;
  device_ = device;
  candidates_ =
      CreateVideoDecoders(allow_hardware, accepted_storage_, device, surface,
                          rotate_surface_frames_);
  return InitializeNextDecoder(error);
}

bool VideoRenderer::InitializeNextDecoder(std::string* error) {
  while (!candidates_.empty()) {
    std::unique_ptr<VideoDecoder> decoder = std::move(candidates_.front());
    candidates_.erase(candidates_.begin());
    std::string decoder_error;
    if (decoder->Initialize(config_, &decoder_error)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        decoder_.swap(decoder);
      }
      decoder_has_output_ = false;
      return true;
    }
    LOGI("%s decoder rejected %s: %s", decoder->name(),
         config_.codec_name.c_str(), decoder_error.c_str());
    *error = decoder_error;
  }
  if (error->empty()) {
    *error = "no video decoder in this build";
  }
  return false;
}

void VideoRenderer::SetVideoSurface(std::shared_ptr<void> surface) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (decoder_) {
    decoder_->SetVideoSurface(std::move(surface));
  }
}

void VideoRenderer::Start() {
  thread_ = std::thread(&VideoRenderer::DecodeLoop, this);
}

void VideoRenderer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    // Queued frames can hold the decoder's output buffers (MediaCodec), and
    // a decoder out of them waits in Decode() for input space; give them
    // back so it returns and the thread can exit.
    algorithm_.Reset();
    selected_.reset();
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
      unconfirmed_packets_.clear();
    }
    uint64_t generation = entry.generation;
    // Excludes the time OnFrame() waits for queue space.
    int64_t waited = 0;
    const int64_t decode_start = MonotonicMicros();
    const bool simulated = simulate_loss_.exchange(false);
    bool ok = !simulated && Decode(entry.packet, generation, &waited);
    const int64_t decode_time = MonotonicMicros() - decode_start - waited;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entry.packet->end_of_stream) {
      // About the last 32 packets.
      average_decode_time_ += (decode_time - average_decode_time_) / 32;
    }
    if (!ok) {
      // Before the first frame, Decode() has already tried the fallbacks.
      if (simulated || (decoder_has_output_ && decoder_->is_hardware())) {
        LOGW("%s decoder lost", decoder_->name());
        decoder_lost_ = true;
      } else {
        LOGE("%s decoder failed", decoder_->name());
        failed_ = true;
      }
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

bool VideoRenderer::Decode(const std::shared_ptr<EncodedPacket>& packet,
                           uint64_t generation,
                           int64_t* waited) {
  auto output = [this, generation, waited](std::shared_ptr<VideoFrame> frame) {
    decoder_has_output_ = true;
    const int64_t start = MonotonicMicros();
    OnFrame(std::move(frame), generation);
    *waited += MonotonicMicros() - start;
  };
  if (!decoder_has_output_) {
    unconfirmed_packets_.push_back(packet);
  }
  bool ok = decoder_->Decode(*packet, output);
  // Frames already shown cannot be decoded again, so only a decoder without
  // output can be replaced. The packets since the last flush start at a
  // keyframe.
  while (!ok && !decoder_has_output_) {
    LOGW("%s decoder failed before its first frame", decoder_->name());
    std::string error;
    if (!InitializeNextDecoder(&error)) {
      return false;
    }
    ok = true;
    for (size_t i = 0; ok && i < unconfirmed_packets_.size(); ++i) {
      ok = decoder_->Decode(*unconfirmed_packets_[i], output);
    }
  }
  if (decoder_has_output_) {
    unconfirmed_packets_.clear();
  }
  return ok;
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

void VideoRenderer::Flush(uint64_t generation,
                          int64_t start_time,
                          bool continue_shown) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (continue_shown) {
      const int64_t shown =
          current_ && !current_stale_ ? FrameEnd(*current_)
                                      : inherited_shown_end_;
      if (shown != kNoTimestamp) {
        start_time = std::max(start_time, shown);
      }
    }
    inherited_shown_end_ = kNoTimestamp;
    generation_ = generation;
    start_time_ = start_time;
    // The frame rate survives a seek; cadence detection need not restart.
    algorithm_.Reset(
        VideoRendererAlgorithm::ResetFlag::kPreserveNextFrameEstimates);
    selected_.reset();
    selected_vsync_ = kNoTimestamp;
    current_stale_ = true;
    first_frame_pending_ = true;
    end_of_stream_ = false;
  }
  condition_.notify_all();
}

int64_t VideoRenderer::shown_end() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (current_ && !current_stale_) {
    return FrameEnd(*current_);
  }
  return inherited_shown_end_;
}

void VideoRenderer::ContinueFrom(int64_t shown_end) {
  std::lock_guard<std::mutex> lock(mutex_);
  inherited_shown_end_ = shown_end;
}

bool VideoRenderer::HasEnoughData() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return algorithm_.effective_frames_queued() > 0 || end_of_stream_;
}

bool VideoRenderer::IsStarved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return algorithm_.effective_frames_queued() == 0 && !end_of_stream_;
}

bool VideoRenderer::HasFramesUntil(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Not while the decoder is lost: time must wait for its replacement.
  if (end_of_stream_ || failed_ ||
      algorithm_.frames_queued() > kMaxQueuedFrames) {
    return true;
  }
  return algorithm_.frames_queued() > 0 &&
         FrameEnd(*algorithm_.last_frame()) > media_time;
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

bool VideoRenderer::DecoderLost() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return decoder_lost_;
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

void VideoRenderer::ExpireFramesWhileHidden(int64_t now) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_pull_ == kNoTimestamp || now - last_pull_ < kHiddenTimeout ||
        algorithm_.frames_queued() < 2) {
      return;
    }
    algorithm_.RemoveExpiredFrames(now);
  }
  // Queue space for the decoder.
  condition_.notify_all();
}

std::shared_ptr<VideoFrame> VideoRenderer::SelectFrameLocked(
    int64_t display_time) {
  const int64_t now = NowMicros();
  // Back from hidden: what ExpireFramesWhileHidden() left is stale, and
  // dropping it is not a playback problem.
  const bool was_hidden =
      last_pull_ != kNoTimestamp && now - last_pull_ >= kHiddenTimeout;
  last_pull_ = now;
  const VsyncEstimator::Vsync vsync = vsync_estimator_.Update(display_time);
  // Already decided for this vsync: has_new_frame and acquire_frame are
  // usually called for the same one.
  if (selected_ && vsync.time == selected_vsync_) {
    return selected_;
  }
  if (algorithm_.frames_queued() == 0) {
    return current_;
  }
  size_t dropped = 0;
  if (first_frame_pending_) {
    // Show the first frame after a flush even if the clock has already
    // passed it (the first pull can come late under load), so a seek always
    // lands on its target frame. An empty interval makes the algorithm
    // return its front frame, as in Chromium's
    // media/renderers/video_renderer_impl.cc PaintFirstFrame().
    first_frame_pending_ = false;
    selected_ = algorithm_.Render(vsync.time, vsync.time, &dropped);
  } else {
    selected_ =
        algorithm_.Render(vsync.time, vsync.time + vsync.interval, &dropped);
  }
  selected_vsync_ = vsync.time;
  if (!was_hidden) {
    frames_dropped_ += static_cast<int64_t>(dropped);
  }
  return selected_;
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
      if (frame->present) {
        frame->present(display_time);
        frame->present = nullptr;
      }
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
  std::lock_guard<std::mutex> lock(mutex_);
  return decoder_ ? decoder_->name() : "none";
}

bool VideoRenderer::is_hardware() const {
  std::lock_guard<std::mutex> lock(mutex_);
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

int64_t VideoRenderer::average_decode_time() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return average_decode_time_;
}

}  // namespace lychee
