#define LOG_TAG "video_renderer"

#include "pipeline/video_renderer.h"

#include <algorithm>
#include <chrono>
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
// A decoder call this long counts as hung. Longer than MediaCodec's wait
// for the last frames at the end of the stream (3 s).
constexpr int64_t kHungDecoderTimeout = 4 * kMicrosPerSecond;
// Frames without a duration are assumed to last this long (30 fps).
constexpr int64_t kDefaultFrameDuration = kMicrosPerSecond / 30;
// Frames ending this little after the start count as ending before it.
// Durations are differences of timestamps rounded to microseconds (from
// 90 kHz in MPEG-TS), so the same picture in two streams, such as the
// frame on screen and its counterpart in an HLS variant switched to, can
// end a microsecond apart.
constexpr int64_t kFrameEndSlop = 100;
int64_t FrameEnd(const VideoFrame& frame) {
  return frame.pts +
         (frame.duration > 0 ? frame.duration : kDefaultFrameDuration);
}

}  // namespace

void VideoRenderer::Deleter::operator()(VideoRenderer* renderer) const {
  renderer->Stop();
  {
    std::lock_guard<std::mutex> lock(renderer->mutex_);
    if (renderer->abandoned_ && !renderer->exited_) {
      renderer->released_ = true;
      return;
    }
  }
  delete renderer;
}

VideoRenderer::Ptr VideoRenderer::Create(MediaClock* clock,
                                         uint32_t accepted_storage,
                                         bool rotate_surface_frames) {
  return Ptr(new VideoRenderer(clock, accepted_storage, rotate_surface_frames));
}

VideoRenderer::VideoRenderer(MediaClock* clock,
                             uint32_t accepted_storage,
                             bool rotate_surface_frames)
    : accepted_storage_(accepted_storage),
      rotate_surface_frames_(rotate_surface_frames),
      converter_(CreateFrameConverter()),
      algorithm_([clock](const std::vector<int64_t>& media_times,
                         std::vector<int64_t>* system_times) {
        return clock->SystemTimesFor(media_times, system_times);
      }),
      created_(NowMicros()) {}

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
  std::unique_lock<std::mutex> lock(mutex_);
  stopped_ = true;
  // Queued frames can hold the decoder's output buffers (MediaCodec), and
  // a decoder out of them waits in Decode() for input space; give them
  // back so it returns and the thread can exit.
  algorithm_.Reset();
  selected_.reset();
  condition_.notify_all();
  if (!thread_.joinable()) {
    return;
  }
  while (!exited_) {
    const int64_t since = in_decoder_since_;
    if (since != 0 && MonotonicMicros() - since >= kHungDecoderTimeout) {
      // After Stop() the thread touches only this renderer, which Deleter
      // then leaves to it.
      LOGW("%s decoder hung; leaving its thread behind", decoder_->name());
      abandoned_ = true;
      condition_.notify_all();
      thread_.detach();
      return;
    }
    condition_.wait_for(lock, std::chrono::milliseconds(20));
  }
  lock.unlock();
  thread_.join();
}

bool VideoRenderer::DecoderHung(bool clock_running) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t now = MonotonicMicros();
  // Since when |condition| has held, from the first call that saw it.
  auto track = [now](bool condition, int64_t* since) {
    if (!condition) {
      *since = kNoTimestamp;
    } else if (*since == kNoTimestamp) {
      *since = now;
    }
  };
  track(algorithm_.effective_frames_queued() <= 1, &drained_since_);
  track(clock_running, &running_since_);
  const int64_t since = in_decoder_since_;
  if (since == 0 || !(simulate_hang_ || decoder_->is_hardware())) {
    return false;
  }
  // Frames waiting to be shown can hold the decoder's output buffers
  // (MediaCodec): a wait counts once they are gone, or while the clock runs
  // and moves them on.
  int64_t free_since = kNoTimestamp;
  for (int64_t time : {drained_since_, running_since_}) {
    if (time != kNoTimestamp &&
        (free_since == kNoTimestamp || time < free_since)) {
      free_since = time;
    }
  }
  return free_since != kNoTimestamp &&
         now - std::max(since, free_since) >= kHungDecoderTimeout;
}

void VideoRenderer::DecodeLoop() {
  SetCurrentThreadName("lychee-vdecode");
  uint64_t decoder_generation = 0;
  bool flush = false;
  PacketQueue::Entry entry;
  while (queue_->Pop(&entry)) {
    in_decoder_since_ = MonotonicMicros();
    if (entry.generation != decoder_generation || flush) {
      decoder_->Flush();
      decoder_generation = entry.generation;
      flush = false;
      unconfirmed_packets_.clear();
    }
    uint64_t generation = entry.generation;
    // A loop: drain the frames of this pass, then start afresh.
    const bool boundary = entry.packet->boundary;
    const int64_t boundary_time = entry.packet->pts;
    if (boundary) {
      entry.packet = EncodedPacket::CreateEndOfStream(entry.packet->stream_index);
      flush = true;
    }
    // Excludes the time OnFrame() waits for queue space.
    int64_t waited = 0;
    const int64_t decode_start = MonotonicMicros();
    bool simulated = simulate_loss_.exchange(false);
    if (simulate_hang_) {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] { return abandoned_; });
      simulated = true;
    }
    bool ok = !simulated && Decode(entry.packet, generation, &waited);
    in_decoder_since_ = 0;
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
    if (generation == generation_) {
      if (boundary) {
        start_time_ = boundary_time;
      } else if (entry.packet->end_of_stream) {
        end_of_stream_ = true;
      }
    }
    if (stopped_) {
      break;
    }
  }
  bool release;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    exited_ = true;
    release = released_;
    // Under the lock: once it is released, Deleter may delete the renderer.
    condition_.notify_all();
  }
  if (release) {
    delete this;
  }
}

bool VideoRenderer::Decode(const std::shared_ptr<EncodedPacket>& packet,
                           uint64_t generation,
                           int64_t* waited) {
  auto output = [this, generation, waited](std::shared_ptr<VideoFrame> frame) {
    decoder_has_output_ = true;
    in_decoder_since_ = 0;
    const int64_t start = MonotonicMicros();
    OnFrame(std::move(frame), generation);
    const int64_t end = MonotonicMicros();
    *waited += end - start;
    in_decoder_since_ = end;
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
    counts_.decoded++;
    // Accurate seek: skip frames that end before the seek target.
    if (start_time_ != kNoTimestamp && frame->pts != kNoTimestamp &&
        FrameEnd(*frame) <= start_time_ + kFrameEndSlop) {
      return;
    }
  }
  frame = Adapt(frame);
  if (!frame) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  waiting_ = true;
  condition_.wait(lock, [&] {
    return stopped_ || generation != generation_ ||
           (algorithm_.frames_queued() <= kMaxQueuedFrames &&
            (end_time_ == kNoTimestamp || frame->pts == kNoTimestamp ||
             frame->pts < end_time_));
  });
  waiting_ = false;
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
  drained_since_ = kNoTimestamp;
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
    end_time_ = kNoTimestamp;
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
    // Frames given back can let a decoder waiting for output buffers go on.
    drained_since_ = kNoTimestamp;
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
  return algorithm_.effective_frames_queued() == 0 && !end_of_stream_ &&
         end_time_ == kNoTimestamp;
}

bool VideoRenderer::HasFramesUntil(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Not while the decoder is lost: time must wait for its replacement.
  if (end_of_stream_ || failed_ ||
      algorithm_.frames_queued() > kMaxQueuedFrames) {
    return true;
  }
  if (end_time_ != kNoTimestamp) {
    // Timestamps rounded to microseconds can leave the last frame ending a
    // microsecond short of the end time.
    media_time = std::min(media_time, end_time_ - kMicrosPerMilli);
  }
  return algorithm_.frames_queued() > 0 &&
         FrameEnd(*algorithm_.last_frame()) > media_time;
}

bool VideoRenderer::Blocked() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return waiting_ || failed_;
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
  if (end_time_ != kNoTimestamp && media_time >= end_time_) {
    return true;
  }
  if (!end_of_stream_) {
    return false;
  }
  // Hidden: nothing will pull the frames left, so the stream ends with the
  // last one. It stays queued, to show when the caller pulls again.
  if (HiddenLocked(NowMicros()) && algorithm_.frames_queued() > 0) {
    return media_time >= FrameEnd(*algorithm_.last_frame());
  }
  if (algorithm_.frames_queued() > 1) {
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

void VideoRenderer::SetEndTime(int64_t time) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    end_time_ = time;
    if (time != kNoTimestamp) {
      algorithm_.RemoveFramesFrom(time);
    }
  }
  // A frame waiting for the end time to go, or for queue space.
  condition_.notify_all();
}

void VideoRenderer::ExpireFramesWhileHidden(int64_t now) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!HiddenLocked(now) || algorithm_.frames_queued() < 2) {
      return;
    }
    algorithm_.RemoveExpiredFrames(now);
  }
  // Queue space for the decoder.
  condition_.notify_all();
}

bool VideoRenderer::HiddenLocked(int64_t now) const {
  return now - (last_pull_ != kNoTimestamp ? last_pull_ : created_) >=
         kHiddenTimeout;
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
    counts_.dropped += static_cast<int64_t>(dropped);
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
      counts_.rendered++;
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

VideoRenderer::Counts VideoRenderer::counts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

int64_t VideoRenderer::average_decode_time() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return average_decode_time_;
}

}  // namespace lychee
