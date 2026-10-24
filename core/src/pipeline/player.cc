#define LOG_TAG "player"

#include "pipeline/player.h"

#include <algorithm>
#include <chrono>

#include "base/log.h"
#include "base/thread_name.h"
#include "media/factory.h"

namespace lychee {

namespace {

// Keep reading until each active stream has this much queued (and after a
// stall, wait for this much before playing again)...
constexpr int64_t kMinQueuedDuration = 3 * kMicrosPerSecond;
// ...unless the queues already hold this many bytes.
constexpr size_t kMaxQueuedBytes = 64 * 1024 * 1024;

const char* StateName(PlayerState state) {
  switch (state) {
    case PlayerState::kIdle:
      return "idle";
    case PlayerState::kLoading:
      return "loading";
    case PlayerState::kReady:
      return "ready";
    case PlayerState::kBuffering:
      return "buffering";
    case PlayerState::kEnded:
      return "ended";
    case PlayerState::kError:
      return "error";
  }
  return "unknown";
}

}  // namespace

Player::Player(const PlayerConfig& config) : config_(config) {}

Player::~Player() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  condition_.notify_all();
  if (demuxer_) {
    demuxer_->Abort();
  }
  video_queue_.Abort();
  audio_queue_.Abort();
  if (thread_.joinable()) {
    thread_.join();
  }
  video_for_display_ = nullptr;
  if (video_renderer_) {
    video_renderer_->Stop();
  }
  if (audio_renderer_) {
    audio_renderer_->Stop();
  }
}

void Player::SetEventCallback(EventCallback callback, void* user_data) {
  std::lock_guard<std::mutex> lock(mutex_);
  event_callback_ = callback;
  event_user_data_ = user_data;
}

bool Player::Open(const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != PlayerState::kIdle) {
    return false;
  }
  SetStateLocked(PlayerState::kLoading);
  demuxer_ = CreateDemuxer();
  thread_ = std::thread(&Player::Run, this, url);
  return true;
}

void Player::Play() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::kEnded) {
      seek_pending_ = true;
      seek_target_ = 0;
    }
    if (!play_when_ready_) {
      play_when_ready_ = true;
      PostEventLocked(PlayerEvent::kPlayingChanged, 1);
    }
  }
  condition_.notify_all();
}

void Player::Pause() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (play_when_ready_) {
      play_when_ready_ = false;
      PostEventLocked(PlayerEvent::kPlayingChanged, 0);
    }
  }
  condition_.notify_all();
}

void Player::Seek(int64_t position) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::kIdle || state_ == PlayerState::kError) {
      return;
    }
    position = std::max<int64_t>(position, 0);
    if (media_info_.duration != kNoTimestamp) {
      position = std::min(position, media_info_.duration);
    }
    seek_pending_ = true;
    seek_target_ = position;
    // Do not wait for a read stalled on the network. Not during Prepare():
    // that would abort the open itself.
    if (video_renderer_ || audio_renderer_) {
      demuxer_->Interrupt();
    }
  }
  condition_.notify_all();
}

void Player::SetVolume(float volume) {
  std::lock_guard<std::mutex> lock(mutex_);
  volume_ = volume;
  if (audio_renderer_) {
    audio_renderer_->SetVolume(volume);
  }
}

PlayerState Player::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool Player::playing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return play_when_ready_;
}

int64_t Player::position() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (seek_pending_ || seek_in_progress_) {
    return seek_target_;
  }
  switch (state_) {
    case PlayerState::kIdle:
    case PlayerState::kLoading:
      return 0;
    case PlayerState::kError:
      // Where playback stopped, if it ever started.
      if (!video_renderer_ && !audio_renderer_) {
        return 0;
      }
      break;
    case PlayerState::kEnded:
      if (media_info_.duration != kNoTimestamp) {
        return media_info_.duration;
      }
      break;
    default:
      break;
  }
  int64_t position =
      std::max<int64_t>(clock_.Now() - media_info_.start_time, 0);
  if (media_info_.duration != kNoTimestamp) {
    position = std::min(position, media_info_.duration);
  }
  return position;
}

int64_t Player::duration() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return media_info_.duration;
}

int64_t Player::buffered_position() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!video_renderer_ && !audio_renderer_) {
    return 0;
  }
  int64_t end = kInfiniteTime;
  if (video_renderer_) {
    end = std::min(end, video_last_pts_);
  }
  if (audio_renderer_) {
    end = std::min(end, audio_last_pts_);
  }
  // Data from before a seek target (back to its keyframe) is not ahead.
  int64_t now = seek_pending_ || seek_in_progress_
                    ? media_info_.start_time + seek_target_
                    : clock_.Now();
  if (end == kInfiniteTime || end == kNoTimestamp || end < now) {
    end = now;
  }
  end -= media_info_.start_time;
  if (media_info_.duration != kNoTimestamp) {
    end = std::min(end, media_info_.duration);
  }
  return std::max<int64_t>(end, 0);
}

bool Player::video_size(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // |video_stream_| is valid once the renderer is published.
  if (!video_renderer_) {
    return false;
  }
  *width = video_width_;
  *height = video_height_;
  return true;
}

int Player::video_rotation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!video_renderer_) {
    return 0;
  }
  return media_info_.streams[video_stream_].video.rotation;
}

std::string Player::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

PlayerStats Player::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  PlayerStats stats;
  if (video_renderer_) {
    stats.video_decoder = video_renderer_->decoder_name();
    stats.hardware = video_renderer_->is_hardware();
    stats.frames_rendered = video_renderer_->frames_rendered();
    stats.frames_dropped = video_renderer_->frames_dropped();
  }
  return stats;
}

bool Player::HasNewFrame(int64_t display_time) const {
  VideoRenderer* renderer = video_for_display_.load();
  return renderer && renderer->HasNewFrame(display_time);
}

std::shared_ptr<VideoFrame> Player::AcquireFrame(int64_t display_time) {
  VideoRenderer* renderer = video_for_display_.load();
  return renderer ? renderer->Acquire(display_time) : nullptr;
}

void Player::Run(std::string url) {
  SetCurrentThreadName("lychee-player");
  std::string error;
  bool prepared = Prepare(url, &error);
  if (!prepared) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!quit_) {
      LOGE("open %s failed: %s", url.c_str(), error.c_str());
      FailLocked(error);
    }
  }
  DispatchEvents();
  if (!prepared) {
    return;
  }
  read_thread_ = std::thread(&Player::ReadLoop, this);

  for (;;) {
    bool seek = false;
    int64_t seek_target = 0;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (quit_) {
        break;
      }
      if (seek_pending_) {
        seek = true;
        seek_target = seek_target_;
        seek_pending_ = false;
        seek_in_progress_ = true;
        // Seek() interrupted any read in flight; wait for it to return.
        seeking_ = true;
        condition_.wait(lock, [this] { return !reading_ || quit_; });
        if (quit_) {
          break;
        }
      }
    }
    if (seek) {
      ProcessSeek(seek_target);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        seeking_ = false;
      }
      condition_.notify_all();
    }
    UpdateState();
    DispatchEvents();
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, std::chrono::milliseconds(10),
                        [this] { return quit_ || seek_pending_; });
  }
  read_thread_.join();
}

void Player::ReadLoop() {
  SetCurrentThreadName("lychee-demux");
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      reading_ = false;
      condition_.notify_all();
      // Queue levels change without a notification, so poll.
      condition_.wait_for(lock, std::chrono::milliseconds(10),
                          [this] { return quit_ || ShouldReadLocked(); });
      if (quit_) {
        return;
      }
      if (!ShouldReadLocked()) {
        continue;
      }
      reading_ = true;
    }
    ReadPacket();
  }
}

bool Player::ShouldReadLocked() const {
  return !seek_pending_ && !seeking_ && !end_of_input_ &&
         state_ != PlayerState::kError && NeedMoreData();
}

bool Player::Prepare(const std::string& url, std::string* error) {
  if (!demuxer_) {
    *error = "no demuxer in this build";
    return false;
  }
  if (!demuxer_->Open(url, error)) {
    return false;
  }
  const MediaInfo& info = demuxer_->media_info();
  LOGI("opened %s: format %s, duration %lld us, %zu streams", url.c_str(),
       info.format_name.c_str(), static_cast<long long>(info.duration),
       info.streams.size());

  std::unique_ptr<VideoRenderer> video;
  std::unique_ptr<AudioRenderer> audio;
  std::vector<int> enabled_streams;
  if (info.default_video_stream >= 0) {
    const VideoConfig& config = info.streams[info.default_video_stream].video;
    video = std::make_unique<VideoRenderer>(&clock_, config_.video_frame_types);
    std::string video_error;
    if (video->Initialize(config, config_.hardware_decoding, &video_queue_,
                          &video_error)) {
      video_stream_ = info.default_video_stream;
      enabled_streams.push_back(video_stream_);
    } else {
      LOGW("video disabled: %s", video_error.c_str());
      video.reset();
    }
  }
  if (info.default_audio_stream >= 0) {
    const AudioConfig& config = info.streams[info.default_audio_stream].audio;
    audio = std::make_unique<AudioRenderer>(&clock_);
    std::string audio_error;
    if (audio->Initialize(config, &audio_queue_, &audio_error)) {
      audio_stream_ = info.default_audio_stream;
      enabled_streams.push_back(audio_stream_);
    } else {
      LOGW("audio disabled: %s", audio_error.c_str());
      audio.reset();
    }
  }
  if (!video && !audio) {
    *error = "no playable audio or video stream";
    return false;
  }
  demuxer_->SetEnabledStreams(enabled_streams);
  clock_.Reset(info.start_time);

  std::lock_guard<std::mutex> lock(mutex_);
  media_info_ = info;
  video_renderer_ = std::move(video);
  audio_renderer_ = std::move(audio);
  if (audio_renderer_) {
    audio_renderer_->SetVolume(volume_);
    audio_renderer_->Start();
  }
  if (video_renderer_) {
    video_renderer_->Start();
    video_for_display_ = video_renderer_.get();
  }
  PostEventLocked(PlayerEvent::kMediaInfo,
                  info.duration == kNoTimestamp ? -1 : info.duration);
  if (video_renderer_) {
    const VideoConfig& config = info.streams[video_stream_].video;
    video_width_ = config.display_width();
    video_height_ = config.height;
    PostEventLocked(PlayerEvent::kVideoSizeChanged, video_width_,
                    video_height_);
  }
  return true;
}

bool Player::NeedMoreData() const {
  if (video_queue_.bytes() + audio_queue_.bytes() >= kMaxQueuedBytes) {
    return false;
  }
  if (video_renderer_ && video_queue_.duration() < kMinQueuedDuration) {
    return true;
  }
  if (audio_renderer_ && audio_queue_.duration() < kMinQueuedDuration) {
    return true;
  }
  return false;
}

void Player::ReadPacket() {
  auto packet = std::make_shared<EncodedPacket>();
  DemuxerReadResult result = demuxer_->Read(packet.get());
  if (result == DemuxerReadResult::kAborted) {
    return;
  }
  if (result != DemuxerReadResult::kOk) {
    if (video_renderer_) {
      video_queue_.Push(EncodedPacket::CreateEndOfStream(video_stream_));
    }
    if (audio_renderer_) {
      audio_queue_.Push(EncodedPacket::CreateEndOfStream(audio_stream_));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    end_of_input_ = true;
    if (result == DemuxerReadResult::kError) {
      input_error_ = demuxer_->error();
      LOGW("input failed (%s); playing what was read", input_error_.c_str());
    }
    if (media_info_.duration != kNoTimestamp) {
      video_last_pts_ = audio_last_pts_ =
          media_info_.start_time + media_info_.duration;
    }
    return;
  }

  int64_t pts = packet->pts;
  if (packet->stream_index == video_stream_) {
    video_queue_.Push(std::move(packet));
    if (pts != kNoTimestamp) {
      std::lock_guard<std::mutex> lock(mutex_);
      video_last_pts_ = std::max(video_last_pts_, pts);
    }
  } else if (packet->stream_index == audio_stream_) {
    audio_queue_.Push(std::move(packet));
    if (pts != kNoTimestamp) {
      std::lock_guard<std::mutex> lock(mutex_);
      audio_last_pts_ = std::max(audio_last_pts_, pts);
    }
  }
}

void Player::ProcessSeek(int64_t target) {
  int64_t media_target = media_info_.start_time + target;
  LOGI("seek to %lld us", static_cast<long long>(target));
  demuxer_->Seek(media_target);
  generation_++;
  video_queue_.Flush(generation_);
  audio_queue_.Flush(generation_);
  if (video_renderer_) {
    video_renderer_->Flush(generation_, media_target);
  }
  if (audio_renderer_) {
    audio_renderer_->Flush(generation_, media_target);
  }

  std::lock_guard<std::mutex> lock(mutex_);
  end_of_input_ = false;
  rebuffering_ = false;
  input_error_.clear();
  clock_.Reset(media_target);
  clock_running_ = false;
  if (audio_renderer_) {
    audio_renderer_->SetPlaying(false);
  }
  video_last_pts_ = audio_last_pts_ = kNoTimestamp;
  if (state_ != PlayerState::kLoading) {
    SetStateLocked(PlayerState::kBuffering);
  }
}

void Player::UpdateState() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != PlayerState::kError) {
    if (video_renderer_ && video_renderer_->HasError()) {
      FailLocked(std::string("video decoding failed (") +
                 video_renderer_->decoder_name() + ")");
    } else if (audio_renderer_ && audio_renderer_->HasError()) {
      FailLocked("audio decoding failed");
    }
  }
  switch (state_) {
    case PlayerState::kLoading:
    case PlayerState::kBuffering:
      if (PrerollDoneLocked() &&
          (!rebuffering_ || end_of_input_ || !NeedMoreData())) {
        rebuffering_ = false;
        SetStateLocked(PlayerState::kReady);
        if (seek_in_progress_) {
          seek_in_progress_ = false;
          PostEventLocked(PlayerEvent::kSeekCompleted, seek_target_);
        }
      }
      break;
    case PlayerState::kReady:
      if (EndedLocked()) {
        if (input_error_.empty()) {
          SetStateLocked(PlayerState::kEnded);
        } else {
          FailLocked(input_error_);
        }
      } else if (clock_running_ && StarvedLocked()) {
        rebuffering_ = true;
        SetStateLocked(PlayerState::kBuffering);
      }
      break;
    default:
      break;
  }
  ApplyPlayingLocked();
  UpdateVideoSizeLocked();
}

void Player::UpdateVideoSizeLocked() {
  int width;
  int height;
  if (!video_renderer_ || !video_renderer_->shown_size(&width, &height)) {
    return;
  }
  // The container's pixel aspect ratio still applies.
  VideoConfig shown = media_info_.streams[video_stream_].video;
  shown.width = width;
  shown.height = height;
  width = shown.display_width();
  if (width != video_width_ || height != video_height_) {
    LOGI("video size %dx%d -> %dx%d", video_width_, video_height_, width,
         height);
    video_width_ = width;
    video_height_ = height;
    PostEventLocked(PlayerEvent::kVideoSizeChanged, width, height);
  }
}

void Player::ApplyPlayingLocked() {
  bool run = play_when_ready_ && state_ == PlayerState::kReady;
  if (run == clock_running_) {
    return;
  }
  clock_running_ = run;
  if (run) {
    clock_.Play();
  } else {
    clock_.Pause();
  }
  if (audio_renderer_) {
    audio_renderer_->SetPlaying(run);
  }
}

bool Player::PrerollDoneLocked() const {
  return (!audio_renderer_ || audio_renderer_->HasEnoughData()) &&
         (!video_renderer_ || video_renderer_->HasEnoughData());
}

bool Player::EndedLocked() const {
  return (!audio_renderer_ || audio_renderer_->Ended()) &&
         (!video_renderer_ || video_renderer_->Ended(clock_.Now()));
}

bool Player::StarvedLocked() const {
  if (end_of_input_) {
    return false;
  }
  return (audio_renderer_ && audio_renderer_->IsStarved() &&
          audio_queue_.empty()) ||
         (video_renderer_ && video_renderer_->IsStarved() &&
          video_queue_.empty());
}

void Player::SetStateLocked(PlayerState state) {
  if (state_ == state) {
    return;
  }
  LOGD("state %s -> %s", StateName(state_), StateName(state));
  state_ = state;
  PostEventLocked(PlayerEvent::kStateChanged, static_cast<int64_t>(state));
}

void Player::PostEventLocked(PlayerEvent type, int64_t arg0, int64_t arg1) {
  events_.push_back({type, arg0, arg1});
}

void Player::DispatchEvents() {
  std::vector<Event> events;
  EventCallback callback;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events.swap(events_);
    callback = event_callback_;
    user_data = event_user_data_;
  }
  if (!callback) {
    return;
  }
  for (const Event& event : events) {
    callback(user_data, static_cast<int32_t>(event.type), event.arg0,
             event.arg1);
  }
}

void Player::FailLocked(const std::string& error) {
  error_ = error;
  SetStateLocked(PlayerState::kError);
  PostEventLocked(PlayerEvent::kError);
}

}  // namespace lychee
