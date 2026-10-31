#define LOG_TAG "player"

#include "pipeline/player.h"

#include <algorithm>
#include <cmath>
#include <cctype>
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
constexpr size_t kMaxQueuedBytes = size_t{64} * 1024 * 1024;
// A loop trims at most this much from the container's start and end where
// the tracks start later or end sooner: codec priming and padding (AAC's
// 1024 samples, Opus's pre-skip, a duration rounded up), not content.
constexpr int64_t kMaxLoopTrim = 100 * kMicrosPerMilli;
// Playback logs its stats this often.
constexpr int64_t kStatsLogInterval = 5 * kMicrosPerSecond;

long long Millis(int64_t micros) {
  return static_cast<long long>(micros / kMicrosPerMilli);
}

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

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

// The player whose control thread this is. Event callbacks run there, and
// Open() from one would have to join the thread it runs on.
thread_local const Player* t_control_thread_player = nullptr;

}  // namespace

Player::Player(const PlayerConfig& config)
    : config_(config), video_device_(config.video_device) {}

Player::~Player() {
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  StopPipeline();
}

void Player::StopPipeline() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  condition_.notify_all();
  {
    std::lock_guard<std::mutex> lock(display_mutex_);
    video_for_display_ = nullptr;
  }
  // Only Open() and the destructor replace |demuxer_|, and they hold
  // |open_mutex_|.
  if (demuxer_) {
    demuxer_->Abort();
  }
  video_queue_.Abort();
  audio_queue_.Abort();
  if (thread_.joinable()) {
    thread_.join();
  }
  // A track change in flight may have reset the queues for a new renderer.
  video_queue_.Abort();
  audio_queue_.Abort();
  // A stop fades out like a pause. The control thread is gone, so nothing
  // else calls into the renderers.
  for (auto& retired : retired_audio_) {
    retired->FadeOutForStop();
    retired->Stop();
  }
  retired_audio_.clear();
  if (audio_renderer_) {
    audio_renderer_->FadeOutForStop();
  }
  if (video_renderer_) {
    video_renderer_->Stop();
  }
  if (audio_renderer_) {
    audio_renderer_->Stop();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != PlayerState::kIdle) {
      EndRebufferLocked();
      const PlayerStats stats = StatsLocked();
      LOGI(
          "closed after %lld ms: video %lld decoded, %lld rendered, %lld "
          "dropped; read video %lld kB, audio %lld kB; startup %lld ms; "
          "%lld rebuffers, %lld ms; %lld seeks",
          Millis(MonotonicMicros() - open_time_),
          static_cast<long long>(stats.frames_decoded),
          static_cast<long long>(stats.frames_rendered),
          static_cast<long long>(stats.frames_dropped),
          static_cast<long long>(stats.video_bytes_read / 1000),
          static_cast<long long>(stats.audio_bytes_read / 1000),
          Millis(stats.startup_time),
          static_cast<long long>(stats.rebuffer_count),
          Millis(stats.rebuffer_time),
          static_cast<long long>(stats.seek_count));
    }
  }

  VideoRenderer::Ptr video;
  std::unique_ptr<AudioRenderer> audio;
  std::map<int, std::unique_ptr<SubtitleRenderer>> subtitles;
  std::unique_ptr<Demuxer> demuxer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    video = std::move(video_renderer_);
    audio = std::move(audio_renderer_);
    subtitles = std::move(subtitle_renderers_);
    subtitle_renderers_.clear();
    histories_.clear();
    demuxer = std::move(demuxer_);
    live_ = false;
    live_demuxer_ = nullptr;
    live_change_ = LiveChange();
    video_stream_ = audio_stream_ = subtitle_stream_ = -1;
    generation_ = 0;
    events_.clear();
    state_ = PlayerState::kIdle;
    clock_running_ = false;
    quit_ = false;
    seek_pending_ = false;
    seek_in_progress_ = false;
    seek_target_ = 0;
    report_seek_ = false;
    current_seek_reported_ = false;
    seeking_ = false;
    reading_ = false;
    end_of_input_ = false;
    rebuffering_ = false;
    loop_seek_pending_ = false;
    prerolling_ = false;
    loops_ = 0;
    loop_start_ = loop_length_ = 0;
    spans_.clear();
    loop_end_ = kNoTimestamp;
    track_request_ = TrackRequest();
    input_error_.clear();
    error_.clear();
    media_info_ = MediaInfo();
    video_last_pts_ = audio_last_pts_ = kNoTimestamp;
    video_width_ = video_height_ = 0;
    subtitle_text_.clear();
    subtitle_images_key_ = 0;
    video_decoder_losses_ = 0;
  }
  video_queue_.Reset(0);
  audio_queue_.Reset(0);
  clock_.Reset(0);
  live_audio_.output_lead = 0;
  live_audio_.late = 0;
  live_audio_.silence = 0;
  stats_log_at_ = kNoTimestamp;
  stats_logged_ = PlayerStats();
  retired_video_ = VideoRenderer::Counts();
  open_time_ = MonotonicMicros();
  startup_time_ = 0;
  rebuffer_start_ = kNoTimestamp;
  rebuffer_count_ = rebuffer_time_ = 0;
  seek_request_time_ = kNoTimestamp;
  seek_count_ = last_seek_time_ = 0;
  video_bytes_read_ = 0;
  audio_bytes_read_ = 0;
  download_rate_ = -1;
}

void Player::SetEventCallback(EventCallback callback, void* user_data) {
  std::unique_lock<std::mutex> lock(mutex_);
  event_callback_ = callback;
  event_user_data_ = user_data;
  // The events being handed out use the old callback. From inside it,
  // waiting would never end.
  if (t_control_thread_player != this) {
    condition_.wait(lock, [this] { return !dispatching_; });
  }
}

bool Player::OnControlThread() const {
  if (t_control_thread_player != this) {
    return false;
  }
  LOGE("open from an event callback: call it from another thread");
  return true;
}

bool Player::Open(const std::string& url) {
  if (url.empty() || OnControlThread()) {
    return false;
  }
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  StopPipeline();
  std::lock_guard<std::mutex> lock(mutex_);
  SetStateLocked(PlayerState::kLoading);
  demuxer_ = CreateDemuxer();
  thread_ = std::thread(&Player::Run, this, url);
  return true;
}

bool Player::OpenLive(int64_t latency) {
  if (OnControlThread()) {
    return false;
  }
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  StopPipeline();
  std::lock_guard<std::mutex> lock(mutex_);
  SetStateLocked(PlayerState::kLoading);
  auto demuxer =
      std::make_unique<LiveDemuxer>(latency, &live_audio_.output_lead);
  live_demuxer_ = demuxer.get();
  demuxer_ = std::move(demuxer);
  live_ = true;
  LOGI("live input, latency %lld ms", static_cast<long long>(latency / 1000));
  thread_ = std::thread(&Player::Run, this, std::string("live"));
  return true;
}

namespace {

int LiveStream(MediaType type) {
  return type == MediaType::kVideo ? LiveDemuxer::kVideoStream
                                   : LiveDemuxer::kAudioStream;
}

}  // namespace

bool Player::SetLiveTrack(MediaType type, const StreamInfo& info) {
  if (type != MediaType::kVideo && type != MediaType::kAudio) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!live_demuxer_ || state_ == PlayerState::kError) {
      return false;
    }
    const int stream = LiveStream(type);
    live_demuxer_->SetTrack(stream, info);
    live_change_.track[stream] = true;
    live_change_.info[stream] = info;
  }
  condition_.notify_all();
  return true;
}

bool Player::PushLivePacket(MediaType type,
                            int64_t pts,
                            const uint8_t* data,
                            size_t size) {
  if ((type != MediaType::kVideo && type != MediaType::kAudio) || !data ||
      size == 0) {
    return false;
  }
  // The lock keeps the demuxer alive; pushing only copies.
  std::lock_guard<std::mutex> lock(mutex_);
  return live_demuxer_ &&
         live_demuxer_->Push(LiveStream(type), pts, data, size);
}

bool Player::FlushLive(MediaType type) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!live_demuxer_) {
      return false;
    }
    const bool all = type != MediaType::kVideo && type != MediaType::kAudio;
    live_demuxer_->Flush(all ? -1 : LiveStream(type));
    for (int stream : {LiveDemuxer::kVideoStream, LiveDemuxer::kAudioStream}) {
      if (all || stream == LiveStream(type)) {
        live_change_.flush[stream] = true;
      }
    }
  }
  condition_.notify_all();
  return true;
}

void Player::Play() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::kEnded) {
      seek_pending_ = true;
      seek_target_ = 0;
      loop_seek_pending_ = false;
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
    if (state_ == PlayerState::kIdle || state_ == PlayerState::kError ||
        live_) {
      return;
    }
    // Once the input is open, a seek it cannot do is not started: the
    // interrupt alone would cost the read in flight.
    if ((video_renderer_ || audio_renderer_) && !media_info_.seekable) {
      LOGW("seek ignored: the input cannot seek");
      return;
    }
    position = std::max<int64_t>(position, 0);
    if (media_info_.duration != kNoTimestamp) {
      position = std::min(position, media_info_.duration);
    }
    seek_pending_ = true;
    seek_target_ = position;
    report_seek_ = true;
    seek_request_time_ = MonotonicMicros();
    loop_seek_pending_ = false;
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

void Player::SetVideoDevice(VideoDevice device) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device == video_device_) {
      return;
    }
    LOGI("video device changed");
    video_device_ = std::move(device);
    video_decoder_losses_ = 0;
    // No more frames from the old device; the control thread replaces the
    // renderer.
    PublishVideoLocked();
  }
  condition_.notify_all();
}

void Player::RebuildVideoDecoder() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!video_renderer_ || state_ == PlayerState::kError ||
        track_request_.rebuild_video) {
      return;
    }
    // Software by choice or for want of a hardware decoder for the codec:
    // nothing to recover, and a rebuild would only resync. Software after
    // hardware losses is recovered: iOS invalidates VideoToolbox sessions
    // in the background (-12903), so rebuilds there fail into software,
    // and the foreground is when hardware works again.
    if (!video_renderer_->is_hardware() &&
        (!config_.hardware_decoding || video_decoder_losses_ < 2)) {
      return;
    }
    // An explicit platform recovery gets another hardware attempt, like a
    // device change. It must not accumulate losses on every foreground.
    video_decoder_losses_ = 0;
    track_request_.rebuild_video = true;
    demuxer_->Interrupt();
  }
  condition_.notify_all();
}

void Player::SetVideoSurface(void* surface) {
  std::shared_ptr<void> retained = RetainVideoSurface(surface);
  std::lock_guard<std::mutex> lock(mutex_);
  video_surface_ = retained;
  if (video_renderer_) {
    video_renderer_->SetVideoSurface(std::move(retained));
  }
}

void Player::SetRate(double rate) {
  if (!(rate > 0)) {
    return;
  }
  rate = std::clamp(rate, kMinRate, kMaxRate);
  std::lock_guard<std::mutex> lock(mutex_);
  if (rate == rate_) {
    return;
  }
  LOGI("rate %.2f -> %.2f", rate_, rate);
  rate_ = rate;
  if (live_) {
    // For the next file; live input plays in real time.
    return;
  }
  clock_.SetRate(rate);
  if (audio_renderer_) {
    audio_renderer_->SetRate(rate);
  }
}

void Player::SetLooping(bool looping) {
  std::lock_guard<std::mutex> lock(mutex_);
  looping_ = looping;
  ApplyLoopEndLocked();
}

bool Player::SelectTrack(MediaType type, int track) {
  if (type != MediaType::kVideo && type != MediaType::kAudio &&
      type != MediaType::kSubtitle) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::kIdle || state_ == PlayerState::kError ||
        live_ || (!video_renderer_ && !audio_renderer_)) {
      return false;
    }
    int stream = -1;
    if (type == MediaType::kVideo && track >= 0 &&
        config_.video_frame_types == 0) {
      return false;
    }
    if (track >= 0) {
      std::vector<int> streams = TrackStreamsLocked();
      if (track >= static_cast<int>(streams.size())) {
        return false;
      }
      stream = streams[track];
      const StreamInfo& info = media_info_.streams[stream];
      if (info.type != type) {
        return false;
      }
    }
    if (type == MediaType::kSubtitle) {
      // Its stream is read all along: nothing to seek or rebuild.
      if (stream >= 0 && !subtitle_renderers_.count(stream)) {
        return false;
      }
      if (stream != subtitle_stream_) {
        LOGI("subtitle track -> stream %d", stream);
        subtitle_stream_ = stream;
        PostEventLocked(PlayerEvent::kTracksChanged);
        UpdateSubtitleLocked();
      }
      return true;
    }
    int* requested = type == MediaType::kVideo ? &track_request_.video
                                               : &track_request_.audio;
    const int current =
        type == MediaType::kVideo ? video_stream_ : audio_stream_;
    // Keep something to play.
    int video = track_request_.video != kUnchanged ? track_request_.video
                                                    : video_stream_;
    int audio = track_request_.audio != kUnchanged ? track_request_.audio
                                                    : audio_stream_;
    if (type == MediaType::kVideo) {
      video = stream;
    } else if (type == MediaType::kAudio) {
      audio = stream;
    }
    if (video < 0 && audio < 0) {
      return false;
    }
    if (*requested == kUnchanged && stream == current) {
      return true;
    }
    *requested = stream == current ? kUnchanged : stream;
    // Even when this cancels the request: it interrupted the read, which
    // only the control thread's seek resumes.
    track_request_.resync = true;
    demuxer_->Interrupt();
  }
  condition_.notify_all();
  return true;
}

PlayerState Player::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool Player::playing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return play_when_ready_;
}

double Player::rate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rate_;
}

bool Player::looping() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return looping_;
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
  const int64_t now = clock_.Now();
  int64_t position = std::max<int64_t>(
      now - LoopOffsetLocked(now) - media_info_.start_time, 0);
  if (media_info_.duration != kNoTimestamp) {
    position = std::min(position, media_info_.duration);
  }
  return position;
}

bool Player::seekable() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return media_info_.seekable && !live_ && state_ != PlayerState::kError;
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
  // Read into the next pass: buffered to the end of this one.
  end -= LoopOffsetLocked(now) + media_info_.start_time;
  if (media_info_.duration != kNoTimestamp) {
    end = std::min(end, media_info_.duration);
  }
  return std::max<int64_t>(end, 0);
}

bool Player::video_size(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(mutex_);
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
  return StatsLocked();
}

PlayerStats Player::StatsLocked() const {
  PlayerStats stats;
  const int64_t now = clock_.Now();
  stats.frames_rendered = retired_video_.rendered;
  stats.frames_dropped = retired_video_.dropped;
  stats.frames_decoded = retired_video_.decoded;
  if (video_renderer_) {
    stats.video_decoder = video_renderer_->decoder_name();
    stats.hardware = video_renderer_->is_hardware();
    const VideoRenderer::Counts counts = video_renderer_->counts();
    stats.frames_rendered += counts.rendered;
    stats.frames_dropped += counts.dropped;
    stats.frames_decoded += counts.decoded;
    stats.video_decode_time = video_renderer_->average_decode_time();
    if (video_last_pts_ != kNoTimestamp) {
      stats.video_buffered = std::max<int64_t>(video_last_pts_ - now, 0);
    }
  }
  if (audio_renderer_) {
    stats.audio_decoder = audio_renderer_->decoder_name();
    if (audio_last_pts_ != kNoTimestamp) {
      stats.audio_buffered = std::max<int64_t>(audio_last_pts_ - now, 0);
    }
  }
  if (live_demuxer_) {
    stats.live_video_packets_dropped = live_demuxer_->video_packets_dropped();
    stats.live_audio_late = live_audio_.late.load(std::memory_order_relaxed);
    stats.live_audio_silence =
        live_audio_.silence.load(std::memory_order_relaxed);
    stats.live_audio_behind = live_demuxer_->audio_behind();
  }
  stats.video_bytes_read = video_bytes_read_.load(std::memory_order_relaxed);
  stats.audio_bytes_read = audio_bytes_read_.load(std::memory_order_relaxed);
  stats.download_rate = download_rate_.load(std::memory_order_relaxed);
  stats.startup_time = startup_time_;
  stats.rebuffer_count = rebuffer_count_;
  stats.rebuffer_time = rebuffer_time_;
  if (rebuffer_start_ != kNoTimestamp) {
    stats.rebuffer_time += MonotonicMicros() - rebuffer_start_;
  }
  stats.seek_count = seek_count_;
  stats.last_seek_time = last_seek_time_;
  return stats;
}

void Player::LogStatsLocked() {
  const int64_t now = NowMicros();
  if (!live_ && !clock_running_) {
    // Paused, ended, or waiting for data: nothing moves. Count the next
    // interval from when playback runs again.
    stats_log_at_ = kNoTimestamp;
    return;
  }
  if (stats_log_at_ != kNoTimestamp && now < stats_log_at_) {
    return;
  }
  const PlayerStats stats = StatsLocked();
  const LogLevel level = live_ ? LogLevel::kInfo : LogLevel::kDebug;
  if (stats_log_at_ != kNoTimestamp && GetLogLevel() >= level) {
    const PlayerStats& last = stats_logged_;
    const int64_t interval =
        std::max<int64_t>(now - (stats_log_at_ - kStatsLogInterval), 1);
    const double fps =
        static_cast<double>(stats.frames_rendered - last.frames_rendered) *
        kMicrosPerSecond / static_cast<double>(interval);
    const long long video_kbps =
        (stats.video_bytes_read - last.video_bytes_read) * 8 * 1000 / interval;
    const long long audio_kbps =
        (stats.audio_bytes_read - last.audio_bytes_read) * 8 * 1000 / interval;
    if (live_) {
      // Tagged as the live demuxer's lines are.
      LogMessage(
          LogLevel::kInfo, "live",
          "video %s%s %.1f fps, %lld dropped, %lld packets dropped, read %lld "
          "kbit/s; audio %s late %lld ms, silence %lld ms, behind %lld ms; "
          "buffered video %lld ms, audio %lld ms",
          stats.video_decoder, stats.hardware ? " hw" : "", fps,
          static_cast<long long>(stats.frames_dropped - last.frames_dropped),
          static_cast<long long>(stats.live_video_packets_dropped -
                                 last.live_video_packets_dropped),
          video_kbps, stats.audio_decoder,
          Millis(stats.live_audio_late - last.live_audio_late),
          Millis(stats.live_audio_silence - last.live_audio_silence),
          Millis(stats.live_audio_behind), Millis(stats.video_buffered),
          Millis(stats.audio_buffered));
    } else {
      LOGD(
          "video %s%s %.1f fps, %lld dropped, decode %.1f ms, read %lld "
          "kbit/s; "
          "audio %s read %lld kbit/s; buffered video %lld ms, audio %lld ms; "
          "download %lld kbit/s",
          stats.video_decoder, stats.hardware ? " hw" : "", fps,
          static_cast<long long>(stats.frames_dropped - last.frames_dropped),
          static_cast<double>(stats.video_decode_time) / kMicrosPerMilli,
          video_kbps, stats.audio_decoder, audio_kbps,
          Millis(stats.video_buffered), Millis(stats.audio_buffered),
          static_cast<long long>(
              stats.download_rate >= 0 ? stats.download_rate / 1000 : -1));
    }
  }
  stats_logged_ = stats;
  stats_log_at_ = now + kStatsLogInterval;
}

void Player::EndRebufferLocked() {
  if (rebuffer_start_ == kNoTimestamp) {
    return;
  }
  const int64_t duration = MonotonicMicros() - rebuffer_start_;
  rebuffer_time_ += duration;
  rebuffer_start_ = kNoTimestamp;
  LOGI("rebuffered for %lld ms", Millis(duration));
}

std::vector<int> Player::TrackStreamsLocked() const {
  std::vector<int> streams;
  for (const StreamInfo& info : media_info_.streams) {
    if (info.type == MediaType::kVideo || info.type == MediaType::kAudio ||
        info.type == MediaType::kSubtitle) {
      streams.push_back(info.index);
    }
  }
  return streams;
}

std::vector<TrackInfo> Player::tracks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TrackInfo> tracks;
  for (int index : TrackStreamsLocked()) {
    const StreamInfo& info = media_info_.streams[index];
    TrackInfo track;
    track.type = info.type;
    track.stream_index = info.index;
    track.selected = index == video_stream_ || index == audio_stream_ ||
                     index == subtitle_stream_;
    track.language = info.language;
    track.title = info.title;
    switch (info.type) {
      case MediaType::kVideo:
        track.codec = info.video.codec_name;
        track.width = info.video.display_width();
        track.height = info.video.height;
        break;
      case MediaType::kAudio:
        track.codec = info.audio.codec_name;
        track.sample_rate = info.audio.sample_rate;
        track.channels = info.audio.channels;
        break;
      default:
        track.codec = info.subtitle.codec_name;
        break;
    }
    tracks.push_back(std::move(track));
  }
  return tracks;
}

bool Player::metadata(const std::string& key, std::string* value) const {
  const std::string wanted = ToLower(key);
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [tag, tag_value] : media_info_.metadata) {
    if (tag == wanted) {
      *value = tag_value;
      return true;
    }
  }
  return false;
}

std::shared_ptr<const std::vector<uint8_t>> Player::artwork() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return media_info_.artwork;
}

SubtitleRenderer* Player::SelectedSubtitleLocked() const {
  auto it = subtitle_renderers_.find(subtitle_stream_);
  return it == subtitle_renderers_.end() ? nullptr : it->second.get();
}

std::shared_ptr<const SubtitleImages> Player::subtitle_images(
    int64_t display_time,
    int width,
    int height) {
  std::lock_guard<std::mutex> lock(mutex_);
  SubtitleRenderer* renderer = SelectedSubtitleLocked();
  if (!renderer) {
    return nullptr;
  }
  const int64_t media_time =
      display_time ? clock_.MediaTimeAt(display_time) : clock_.Now();
  return renderer->ImagesAt(media_time - LoopOffsetLocked(media_time), width,
                            height, video_width_, video_height_);
}

std::string Player::subtitle(int64_t display_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  SubtitleRenderer* renderer = SelectedSubtitleLocked();
  if (!renderer) {
    return std::string();
  }
  const int64_t media_time =
      display_time ? clock_.MediaTimeAt(display_time) : clock_.Now();
  // Subtitle packets keep the file's timeline, so every pass finds the
  // same cues.
  return renderer->TextAt(media_time - LoopOffsetLocked(media_time));
}

bool Player::HasNewFrame(int64_t display_time) const {
  std::lock_guard<std::mutex> lock(display_mutex_);
  return video_for_display_ && video_for_display_->HasNewFrame(display_time);
}

std::shared_ptr<VideoFrame> Player::AcquireFrame(int64_t display_time) {
  std::lock_guard<std::mutex> lock(display_mutex_);
  return video_for_display_ ? video_for_display_->Acquire(display_time)
                            : nullptr;
}

int64_t Player::UnloopedPts(int64_t pts) const {
  if (pts == kNoTimestamp) {
    return pts;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return pts - LoopOffsetLocked(pts);
}

int64_t Player::LoopOffsetLocked(int64_t media_time) const {
  if (loops_ == 0) {
    return 0;
  }
  // Still the last pass when it ends there.
  if (loop_end_ != kNoTimestamp) {
    media_time = std::min(media_time, loop_end_ - 1);
  }
  // Pass 0 runs from the file's start to |loop_start_| + |loop_length_|.
  const int64_t pass =
      std::clamp<int64_t>((media_time - loop_start_) / loop_length_, 0, loops_);
  return pass * loop_length_;
}

void Player::ApplyLoopEndLocked() {
  // A seek resets |loops_| and flushes the renderers.
  if (seeking_ || seek_pending_) {
    return;
  }
  loop_end_ = kNoTimestamp;
  int64_t end = kNoTimestamp;
  if (!looping_ && loops_ > 0) {
    const int64_t pass = LoopOffsetLocked(clock_.Now()) / loop_length_;
    if (loops_ > pass) {
      end = loop_start_ + (pass + 1) * loop_length_;
    }
  }
  loop_end_ = end;
  if (video_renderer_) {
    video_renderer_->SetEndTime(end);
  }
  if (audio_renderer_) {
    audio_renderer_->SetEndTime(end);
  }
}

void Player::Run(const std::string& url) {
  SetCurrentThreadName("lychee-player");
  t_control_thread_player = this;
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
    bool loop = false;
    bool resync = false;
    int64_t resync_loops = 0;
    int64_t seek_target = 0;
    TrackRequest tracks;
    LiveChange live_change;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (quit_) {
        break;
      }
      if (live_) {
        if (live_change_.pending() || track_request_.pending()) {
          seek = true;
          tracks = track_request_;
          track_request_ = TrackRequest();
          live_change = live_change_;
          live_change_ = LiveChange();
          // A change the caller makes from here on holds the streams
          // again, and this one's Release() leaves them held.
          for (int stream : {LiveDemuxer::kVideoStream,
                             LiveDemuxer::kAudioStream}) {
            live_change.hold[stream] = live_demuxer_->hold(stream);
          }
          seeking_ = true;
          demuxer_->Interrupt();
          condition_.wait(lock, [this] { return !reading_ || quit_; });
          if (quit_) {
            break;
          }
        }
      } else if (seek_pending_ || track_request_.pending()) {
        seek = true;
        tracks = track_request_;
        track_request_ = TrackRequest();
        if (seek_pending_) {
          seek_pending_ = false;
          loop = loop_seek_pending_;
          loop_seek_pending_ = false;
          current_seek_reported_ = report_seek_;
          report_seek_ = false;
        } else if (!seek_in_progress_) {
          // A track change resumes where playback is.
          resync = true;
          const int64_t now = clock_.Now();
          const int64_t offset = LoopOffsetLocked(now);
          resync_loops = offset ? offset / loop_length_ : 0;
          int64_t position = std::max<int64_t>(
              now - offset - media_info_.start_time, 0);
          if (media_info_.duration != kNoTimestamp) {
            position = std::min(position, media_info_.duration);
          }
          seek_target_ = position;
          current_seek_reported_ = false;
        }
        seek_target = seek_target_;
        seek_in_progress_ = !loop;
        // Seek() interrupted any read in flight; wait for it to return.
        seeking_ = true;
        condition_.wait(lock, [this] { return !reading_ || quit_; });
        if (quit_) {
          break;
        }
      }
    }
    if (seek && live_) {
      ApplyLiveChange(tracks, live_change);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        seeking_ = false;
      }
      condition_.notify_all();
    } else if (seek) {
      const bool tracks_changed =
          tracks.tracks_changed() || tracks.rebuild_video;
      if (tracks_changed) {
        ChangeTracks(tracks);
      }
      ProcessSeek(seek_target, loop, resync, resync_loops, tracks_changed);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        seeking_ = false;
      }
      condition_.notify_all();
    }
    UpdateState();
    DispatchEvents();
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait_for(lock, std::chrono::milliseconds(10), [this] {
      return quit_ || seek_pending_ || track_request_.pending() ||
             live_change_.pending();
    });
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
  return !seek_pending_ && !seeking_ && !track_request_.pending() &&
         !end_of_input_ && state_ != PlayerState::kError && NeedMoreData();
}

VideoRenderer::Ptr Player::CreateVideoRenderer(const StreamInfo& info) {
  VideoRenderer::Ptr video = VideoRenderer::Create(
      &clock_, config_.video_frame_types, config_.rotate_surface_frames);
  VideoDevice device;
  std::shared_ptr<void> surface;
  bool hardware;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    device = video_device_;
    surface = video_surface_;
    hardware = config_.hardware_decoding && video_decoder_losses_ < 2;
  }
  std::string error;
  if (!video->Initialize(info.video, hardware, device, surface, &video_queue_,
                         &error)) {
    LOGW("video stream %d disabled: %s", info.index, error.c_str());
    return nullptr;
  }
  return video;
}

std::unique_ptr<AudioRenderer> Player::CreateAudioRenderer(
    const StreamInfo& info) {
  auto audio =
      std::make_unique<AudioRenderer>(&clock_, live_ ? &live_audio_ : nullptr);
  std::string error;
  if (!audio->Initialize(info.audio, &audio_queue_, &error)) {
    LOGW("audio stream %d disabled: %s", info.index, error.c_str());
    return nullptr;
  }
  return audio;
}

std::unique_ptr<SubtitleRenderer> Player::CreateSubtitleRenderer(
    const StreamInfo& info) {
  auto subtitle = std::make_unique<SubtitleRenderer>();
  std::string error;
  if (!subtitle->Initialize(info.subtitle, &error)) {
    LOGW("subtitle stream %d disabled: %s", info.index, error.c_str());
    return nullptr;
  }
  return subtitle;
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
  if (live_) {
    // Tracks come later; time runs from now on.
    clock_.Reset(info.start_time);
    clock_.SetRate(1.0);
    clock_.Play();
    std::lock_guard<std::mutex> lock(mutex_);
    media_info_ = info;
    clock_running_ = true;
    PostEventLocked(PlayerEvent::kMediaInfo, -1);
    return true;
  }
  LOGI("opened %s: format %s, duration %lld us, %zu streams", url.c_str(),
       info.format_name.c_str(), static_cast<long long>(info.duration),
       info.streams.size());

  VideoRenderer::Ptr video;
  std::unique_ptr<AudioRenderer> audio;
  std::map<int, std::unique_ptr<SubtitleRenderer>> subtitles;
  if (info.default_video_stream >= 0 && config_.video_frame_types != 0) {
    video = CreateVideoRenderer(info.streams[info.default_video_stream]);
  }
  if (info.default_audio_stream >= 0) {
    audio = CreateAudioRenderer(info.streams[info.default_audio_stream]);
  }
  if (!video && !audio) {
    *error = "no playable audio or video stream";
    return false;
  }
  for (const StreamInfo& stream : info.streams) {
    if (stream.type == MediaType::kSubtitle) {
      if (auto subtitle = CreateSubtitleRenderer(stream)) {
        subtitles[stream.index] = std::move(subtitle);
      }
    }
  }
  std::map<int, PacketHistory> histories;
  if (!info.seekable) {
    for (const StreamInfo& stream : info.streams) {
      if (stream.type == MediaType::kAudio ||
          (stream.type == MediaType::kVideo && config_.video_frame_types)) {
        histories.try_emplace(stream.index, kMaxQueuedBytes,
                              stream.type == MediaType::kAudio);
      }
    }
  }
  clock_.Reset(info.start_time);

  std::lock_guard<std::mutex> lock(mutex_);
  // A live input before may have left it at 1.
  clock_.SetRate(rate_);
  media_info_ = info;
  histories_ = std::move(histories);
  video_stream_ = video ? info.default_video_stream : -1;
  audio_stream_ = audio ? info.default_audio_stream : -1;
  subtitle_stream_ = subtitles.count(info.default_subtitle_stream)
                         ? info.default_subtitle_stream
                         : -1;
  video_renderer_ = std::move(video);
  audio_renderer_ = std::move(audio);
  subtitle_renderers_ = std::move(subtitles);
  EnableStreamsLocked();
  if (audio_renderer_) {
    audio_renderer_->SetVolume(volume_);
    audio_renderer_->SetRate(rate_);
    audio_renderer_->Start(live_ ? kNoTimestamp : info.start_time);
  }
  if (video_renderer_) {
    video_renderer_->Start();
    PublishVideoLocked();
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

void Player::ChangeTracks(const TrackRequest& request) {
  int video_stream = request.video;
  if (video_stream == kUnchanged && request.rebuild_video) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (video_stream_ >= 0) {
      video_stream = video_stream_;
    }
  }
  if (video_stream != kUnchanged) {
    VideoRenderer::Ptr old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      {
        std::lock_guard<std::mutex> display_lock(display_mutex_);
        video_for_display_ = nullptr;
      }
      old = std::move(video_renderer_);
      video_stream_ = -1;
      if (old) {
        // Nothing pulls from it any more; the counts go on in the next.
        const VideoRenderer::Counts counts = old->counts();
        retired_video_.rendered += counts.rendered;
        retired_video_.dropped += counts.dropped;
        retired_video_.decoded += counts.decoded;
      }
    }
    video_queue_.Abort();
    int64_t shown_end = kNoTimestamp;
    if (old) {
      old->Stop();
      shown_end = old->shown_end();
      old.reset();
    }
    video_queue_.Reset(generation_);
    VideoRenderer::Ptr video;
    if (video_stream >= 0) {
      video = CreateVideoRenderer(media_info_.streams[video_stream]);
    }
    if (video) {
      video->ContinueFrom(shown_end);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int width = video_width_;
    const int height = video_height_;
    if (video) {
      LOGI("video %s -> stream %d (%s)",
           request.video == kUnchanged ? "rebuilt" : "track", video_stream,
           video->decoder_name());
      video_renderer_ = std::move(video);
      video_stream_ = video_stream;
      video_renderer_->Start();
      PublishVideoLocked();
      const VideoConfig& config = media_info_.streams[video_stream_].video;
      video_width_ = config.display_width();
      video_height_ = config.height;
    } else {
      video_width_ = video_height_ = 0;
    }
    if (request.video != kUnchanged || video_width_ != width ||
        video_height_ != height) {
      PostEventLocked(PlayerEvent::kVideoSizeChanged, video_width_,
                      video_height_);
    }
  }
  if (request.audio != kUnchanged) {
    std::unique_ptr<AudioRenderer> old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      old = std::move(audio_renderer_);
      audio_stream_ = -1;
    }
    audio_queue_.Abort();
    if (old) {
      // It fades out while the next renderer starts; UpdateState() stops
      // its sink once the device has played that.
      old->Retire();
      if (old->SinkStopped()) {
        old->Stop();
      } else {
        std::lock_guard<std::mutex> lock(mutex_);
        retired_audio_.push_back(std::move(old));
      }
      old.reset();
    }
    audio_queue_.Reset(generation_);
    std::unique_ptr<AudioRenderer> audio;
    if (request.audio >= 0) {
      audio = CreateAudioRenderer(media_info_.streams[request.audio]);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio) {
      LOGI("audio track -> stream %d", request.audio);
      audio_renderer_ = std::move(audio);
      audio_stream_ = request.audio;
      audio_renderer_->SetVolume(volume_);
      audio_renderer_->SetRate(rate_);
      // The resync seek that follows sets where it starts.
      audio_renderer_->Start(kNoTimestamp);
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  EnableStreamsLocked();
  // A live input may have no tracks for a while.
  if (!video_renderer_ && !audio_renderer_ && !live_) {
    FailLocked("no playable audio or video stream");
  }
  if (request.tracks_changed()) {
    PostEventLocked(PlayerEvent::kTracksChanged);
  }
}

void Player::EnableStreamsLocked() {
  std::vector<int> streams;
  for (int stream : {video_stream_, audio_stream_}) {
    if (stream >= 0) {
      streams.push_back(stream);
    }
  }
  for (const auto& [stream, renderer] : subtitle_renderers_) {
    streams.push_back(stream);
  }
  // HLS fetches each variant on its own: reading the other video streams
  // there would download every variant.
  const bool hls = media_info_.format_name == "hls";
  for (auto& [stream, history] : histories_) {
    if (stream == video_stream_ || stream == audio_stream_) {
      continue;
    }
    if (hls && media_info_.streams[stream].type == MediaType::kVideo) {
      // Read again later, it would have a gap.
      history.Clear();
    } else {
      streams.push_back(stream);
    }
  }
  demuxer_->SetEnabledStreams(streams);
}

void Player::PublishVideoLocked() {
  if (video_renderer_) {
    // The surface may have changed since the renderer was created.
    video_renderer_->SetVideoSurface(video_surface_);
  }
  std::lock_guard<std::mutex> lock(display_mutex_);
  video_for_display_ =
      video_renderer_ && video_renderer_->device() == video_device_
          ? video_renderer_.get()
          : nullptr;
}

void Player::CheckVideoRendererLocked() {
  if (!video_renderer_ || state_ == PlayerState::kError ||
      track_request_.rebuild_video || track_request_.video != kUnchanged) {
    return;
  }
  if (video_renderer_->DecoderLost()) {
    video_decoder_losses_++;
    LOGW("video decoder lost (%d since open); rebuilding at the position",
         video_decoder_losses_);
  } else if (video_renderer_->DecoderHung(clock_running_)) {
    // Another hardware decoder would likely hang the same way.
    video_decoder_losses_ = std::max(video_decoder_losses_, 2);
    LOGW("video decoder hung; rebuilding in software at the position");
  } else if (video_renderer_->device() == video_device_) {
    return;
  }
  track_request_.rebuild_video = true;
  demuxer_->Interrupt();
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
  if (result == DemuxerReadResult::kEndOfStream && WrapLoop()) {
    return;
  }
  // The read thread is the only writer while it runs.
  const int64_t offset = loops_ ? loops_ * loop_length_ : 0;
  if (result != DemuxerReadResult::kOk) {
    if (video_stream_ >= 0) {
      video_queue_.Push(EncodedPacket::CreateEndOfStream(video_stream_));
    }
    if (audio_stream_ >= 0) {
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
          media_info_.start_time + media_info_.duration + offset;
    }
    return;
  }

  if (packet->pts != kNoTimestamp && (packet->stream_index == video_stream_ ||
                                      packet->stream_index == audio_stream_)) {
    StreamSpan& span = spans_[packet->stream_index];
    if (span.first == kNoTimestamp || packet->pts < span.first) {
      span.first = packet->pts;
    }
    const int64_t end = packet->pts + std::max<int64_t>(packet->duration, 0);
    if (span.end == kNoTimestamp || end >= span.end) {
      span.end = end;
      span.open_end = packet->duration <= 0;
    }
  }
  if (offset &&
      (packet->stream_index == video_stream_ ||
       packet->stream_index == audio_stream_)) {
    if (packet->pts != kNoTimestamp) {
      packet->pts += offset;
    }
    if (packet->dts != kNoTimestamp) {
      packet->dts += offset;
    }
  }
  if (auto it = histories_.find(packet->stream_index); it != histories_.end()) {
    it->second.Push(packet, clock_.Now());
  }
  download_rate_.store(demuxer_->download_rate(), std::memory_order_relaxed);
  int64_t pts = packet->pts;
  if (packet->stream_index == video_stream_) {
    video_bytes_read_.fetch_add(static_cast<int64_t>(packet->size()),
                                std::memory_order_relaxed);
    video_queue_.Push(std::move(packet));
    if (pts != kNoTimestamp) {
      std::lock_guard<std::mutex> lock(mutex_);
      video_last_pts_ = std::max(video_last_pts_, pts);
    }
  } else if (packet->stream_index == audio_stream_) {
    audio_bytes_read_.fetch_add(static_cast<int64_t>(packet->size()),
                                std::memory_order_relaxed);
    audio_queue_.Push(std::move(packet));
    if (pts != kNoTimestamp) {
      std::lock_guard<std::mutex> lock(mutex_);
      audio_last_pts_ = std::max(audio_last_pts_, pts);
    }
  } else if (auto it = subtitle_renderers_.find(packet->stream_index);
             it != subtitle_renderers_.end()) {
    it->second->Decode(*packet);
  }
  if (live_demuxer_) {
    live_demuxer_->PacketDelivered();
  }
}

bool Player::WrapLoop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A pending seek or track change replaces the loop; the control thread
    // flushes whatever this pushes.
    if (!looping_ || live_ || !media_info_.seekable ||
        media_info_.duration == kNoTimestamp || media_info_.duration <= 0 ||
        seek_pending_ || track_request_.pending() || quit_) {
      return false;
    }
  }
  if (!demuxer_->Seek(media_info_.start_time)) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Preserve the pass already queued, but do not treat a failed seek as
    // EOF and retry it from the control thread. A pending request or
    // shutdown may have interrupted this read-ahead seek intentionally.
    if (!quit_ && !seek_pending_ && !track_request_.pending()) {
      input_error_ = "loop seek failed";
    }
    return false;
  }
  if (loops_ == 0) {
    SetLoopSpan();
  }
  // Each pass lasts |loop_length_|: audio past it (encoder padding) is
  // cut, and a shorter track is padded with silence, so passes neither
  // overlap nor leave gaps.
  const int64_t boundary = loop_start_ + (loops_ + 1) * loop_length_;
  if (video_stream_ >= 0) {
    video_queue_.Push(EncodedPacket::CreateBoundary(video_stream_, boundary));
  }
  if (audio_stream_ >= 0) {
    audio_queue_.Push(EncodedPacket::CreateBoundary(audio_stream_, boundary));
  }
  for (const auto& [stream, subtitle] : subtitle_renderers_) {
    subtitle->Flush();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loops_++;
  video_last_pts_ = std::max(video_last_pts_, boundary);
  audio_last_pts_ = std::max(audio_last_pts_, boundary);
  LOGI("loop: reading pass %lld", static_cast<long long>(loops_));
  // Looping went off since the check above.
  if (!looping_) {
    ApplyLoopEndLocked();
  }
  return true;
}

void Player::SetLoopSpan() {
  // The container's duration may count codec padding past the tracks'
  // end (Matroska and WebM written by FFmpeg: Duration on the block
  // timeline, rounded up), and audio may start with priming before the
  // video (MPEG-TS, which has no edit list). Each would play as silence
  // at every seam, so later passes take the span all selected tracks
  // cover, when it differs by padding alone. The first pass keeps the
  // file's own start.
  const int64_t file_start = media_info_.start_time;
  const int64_t file_end = file_start + media_info_.duration;
  int64_t start = file_start;
  int64_t end = kInfiniteTime;
  bool start_known = true;
  bool end_known = true;
  for (int stream : {video_stream_, audio_stream_}) {
    if (stream < 0) {
      continue;
    }
    auto it = spans_.find(stream);
    if (it == spans_.end()) {
      start_known = end_known = false;
      break;
    }
    start = std::max(start, it->second.first);
    if (it->second.open_end) {
      end_known = false;
    } else {
      end = std::min(end, it->second.end);
    }
  }
  int64_t loop_start = file_start;
  int64_t loop_end = file_end;
  if (start_known && start - file_start <= kMaxLoopTrim) {
    loop_start = start;
  }
  if (end_known && end < file_end && file_end - end <= kMaxLoopTrim) {
    loop_end = end;
  }
  if (loop_end - loop_start <= 0) {
    loop_start = file_start;
    loop_end = file_end;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loop_start_ = loop_start;
  loop_length_ = loop_end - loop_start;
  if (loop_start != file_start || loop_end != file_end) {
    LOGI("loop: passes span %lld..%lld us of %lld..%lld",
         static_cast<long long>(loop_start), static_cast<long long>(loop_end),
         static_cast<long long>(file_start), static_cast<long long>(file_end));
  }
}

void Player::ProcessSeek(int64_t target,
                         bool loop,
                         bool resync,
                         int64_t loops,
                         bool tracks_changed) {
  const int64_t file_target = media_info_.start_time + target;
  // A resync stays in the pass playing; the read thread offsets its packets
  // by as much.
  const int64_t media_target = file_target + (loops ? loops * loop_length_ : 0);
  LOGI("%s to %lld us", loop ? "loop" : "seek", static_cast<long long>(target));
  const bool user_seek = !loop && !resync && !tracks_changed;
  if (!media_info_.seekable) {
    // A user seek queued before the open finished cannot be checked until
    // the input's capabilities are known. Ignore it.
    if (user_seek) {
      LOGW("seek ignored: the input cannot seek");
      std::lock_guard<std::mutex> lock(mutex_);
      seek_in_progress_ = false;
      current_seek_reported_ = false;
      return;
    }
    // A resync goes on from where the input is. The seek fails without
    // reading; it only clears the interrupt that stopped the read.
    demuxer_->Seek(file_target);
  } else if (!demuxer_->Seek(file_target)) {
    std::lock_guard<std::mutex> lock(mutex_);
    seek_in_progress_ = false;
    current_seek_reported_ = false;
    // A newer request or shutdown can interrupt this seek. Otherwise the
    // demuxer's read position is no longer trustworthy, even when the
    // renderers still have buffered data from before the seek.
    if (!quit_ && !seek_pending_ && !track_request_.pending() &&
        state_ != PlayerState::kError) {
      FailLocked("seek failed");
    }
    return;
  }
  generation_++;
  video_queue_.Flush(generation_);
  audio_queue_.Flush(generation_);
  if (video_renderer_) {
    video_renderer_->Flush(generation_, media_target, resync);
  }
  if (audio_renderer_) {
    audio_renderer_->Flush(generation_, media_target);
  }
  for (const auto& [stream, subtitle] : subtitle_renderers_) {
    subtitle->Flush();
  }
  // Input that cannot seek: the renderers start over from the packets kept
  // since the keyframe before the position, then the read goes on.
  auto replay = [this](int stream, PacketQueue* queue) {
    int64_t last_pts = kNoTimestamp;
    if (auto it = histories_.find(stream); it != histories_.end()) {
      for (const std::shared_ptr<EncodedPacket>& packet :
           it->second.packets()) {
        queue->Push(packet);
        if (packet->pts != kNoTimestamp) {
          last_pts = std::max(last_pts, packet->pts);
        }
      }
    }
    return last_pts;
  };
  const int64_t video_last_pts = replay(video_stream_, &video_queue_);
  const int64_t audio_last_pts = replay(audio_stream_, &audio_queue_);

  std::lock_guard<std::mutex> lock(mutex_);
  end_of_input_ = false;
  rebuffering_ = false;
  EndRebufferLocked();
  input_error_.clear();
  loops_ = loops;
  loop_end_ = kNoTimestamp;
  clock_.Reset(media_target);
  clock_running_ = false;
  if (audio_renderer_) {
    audio_renderer_->SetPlaying(false);
  }
  video_last_pts_ = video_last_pts;
  audio_last_pts_ = audio_last_pts;
  prerolling_ = loop && state_ == PlayerState::kReady;
  if (!prerolling_ && state_ != PlayerState::kLoading &&
      state_ != PlayerState::kError) {
    SetStateLocked(PlayerState::kBuffering);
  }
}

void Player::FinishSeekLocked() {
  if (!seek_in_progress_) {
    return;
  }
  seek_in_progress_ = false;
  if (current_seek_reported_) {
    current_seek_reported_ = false;
    seek_count_++;
    if (seek_request_time_ != kNoTimestamp) {
      last_seek_time_ = MonotonicMicros() - seek_request_time_;
    }
    LOGI("seek to %lld us done in %lld ms",
         static_cast<long long>(seek_target_), Millis(last_seek_time_));
    PostEventLocked(PlayerEvent::kSeekCompleted, seek_target_);
  }
}

void Player::UpdateState() {
  std::lock_guard<std::mutex> lock(mutex_);
  CheckVideoRendererLocked();
  if (video_renderer_) {
    video_renderer_->ExpireFramesWhileHidden(NowMicros());
  }
  if (audio_renderer_) {
    audio_renderer_->PullForStalledDevice(NowMicros());
    audio_renderer_->StopSinkAfterFadeOut(NowMicros());
  }
  for (auto it = retired_audio_.begin(); it != retired_audio_.end();) {
    (*it)->StopSinkAfterFadeOut(NowMicros());
    if ((*it)->SinkStopped()) {
      (*it)->Stop();
      it = retired_audio_.erase(it);
    } else {
      ++it;
    }
  }
  if (state_ != PlayerState::kError) {
    if (video_renderer_ && video_renderer_->HasError()) {
      FailLocked(std::string("video decoding failed (") +
                 video_renderer_->decoder_name() + ")");
    } else if (audio_renderer_ && audio_renderer_->HasError()) {
      FailLocked("audio decoding failed");
    }
  }
  if (live_) {
    // Never buffering or ended: there is nothing to wait for but the
    // sender.
    if (state_ == PlayerState::kLoading &&
        ((video_renderer_ && video_renderer_->HasEnoughData()) ||
         (audio_renderer_ && audio_renderer_->buffered_duration() > 0))) {
      SetStateLocked(PlayerState::kReady);
    }
    UpdateVideoSizeLocked();
    LogStatsLocked();
    return;
  }
  switch (state_) {
    case PlayerState::kLoading:
    case PlayerState::kBuffering:
      if (PrerollDoneLocked() &&
          (!rebuffering_ || end_of_input_ || !NeedMoreData())) {
        rebuffering_ = false;
        EndRebufferLocked();
        SetStateLocked(PlayerState::kReady);
        FinishSeekLocked();
      }
      break;
    case PlayerState::kReady:
      if (prerolling_) {
        if (PrerollDoneLocked()) {
          prerolling_ = false;
          FinishSeekLocked();
        }
      } else if (seek_pending_) {
        // Wait for the seek; the ended and starved checks see stale data.
      } else if (EndedLocked()) {
        if (!input_error_.empty()) {
          FailLocked(input_error_);
        } else if (looping_ && media_info_.seekable) {
          seek_pending_ = true;
          seek_target_ = 0;
          loop_seek_pending_ = true;
          report_seek_ = false;
        } else {
          SetStateLocked(PlayerState::kEnded);
        }
      } else if (clock_running_ && StarvedLocked()) {
        rebuffering_ = true;
        rebuffer_count_++;
        rebuffer_start_ = MonotonicMicros();
        const int64_t now = clock_.Now();
        LOGI("rebuffering at %lld ms",
             Millis(now - LoopOffsetLocked(now) - media_info_.start_time));
        SetStateLocked(PlayerState::kBuffering);
      }
      break;
    default:
      break;
  }
  ApplyPlayingLocked();
  UpdateVideoSizeLocked();
  UpdateSubtitleLocked();
  LogStatsLocked();
}

bool Player::ShownSizeLocked(int* width, int* height) const {
  if (!video_renderer_ || !video_renderer_->shown_size(width, height)) {
    return false;
  }
  // The container's pixel aspect ratio still applies.
  VideoConfig shown = media_info_.streams[video_stream_].video;
  shown.width = *width;
  shown.height = *height;
  *width = shown.display_width();
  return true;
}

void Player::UpdateVideoSizeLocked() {
  int width;
  int height;
  if (!ShownSizeLocked(&width, &height)) {
    return;
  }
  if (width != video_width_ || height != video_height_) {
    LOGI("video size %dx%d -> %dx%d", video_width_, video_height_, width,
         height);
    video_width_ = width;
    video_height_ = height;
    PostEventLocked(PlayerEvent::kVideoSizeChanged, width, height);
  }
}

void Player::UpdateSubtitleLocked() {
  std::string text;
  uint64_t images = 0;
  if (SubtitleRenderer* renderer = SelectedSubtitleLocked()) {
    // Keep the old text until a seek lands.
    if (seek_pending_ || seek_in_progress_ || prerolling_) {
      return;
    }
    const int64_t now = clock_.Now() - LoopOffsetLocked(clock_.Now());
    text = renderer->TextAt(now);
    images = renderer->ImagesKeyAt(now);
  }
  if (text != subtitle_text_ || images != subtitle_images_key_) {
    subtitle_text_ = std::move(text);
    subtitle_images_key_ = images;
    PostEventLocked(PlayerEvent::kSubtitleChanged,
                    static_cast<int64_t>(subtitle_text_.size()),
                    images != 0 ? 1 : 0);
  }
}

void Player::ApplyPlayingLocked() {
  if (live_) {
    return;
  }
  bool run =
      play_when_ready_ && state_ == PlayerState::kReady && !prerolling_;
  if (run == clock_running_) {
    return;
  }
  clock_running_ = run;
  if (run) {
    clock_.Play(audio_renderer_ != nullptr);
  } else {
    clock_.Pause();
  }
  if (audio_renderer_) {
    audio_renderer_->SetPlaying(run);
  }
}

bool Player::SettledForTesting(int64_t until) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // The control thread hands out events every 10 ms of real time, which
  // can be seconds of virtual time: wait for it, or the caller sees them
  // late, as a cue shown from a time past its start.
  if (!events_.empty() || dispatching_) {
    return false;
  }
  if (live_) {
    return LiveSettledLocked();
  }
  switch (state_) {
    case PlayerState::kLoading:
    case PlayerState::kBuffering:
      return false;
    case PlayerState::kReady:
      break;
    default:
      return true;
  }
  if (seek_pending_ || seek_in_progress_ || seeking_ || prerolling_ ||
      track_request_.pending()) {
    return false;
  }
  if (!play_when_ready_) {
    return true;
  }
  // The control thread has yet to start the clock, or to end or loop.
  if (!clock_running_ || EndedLocked()) {
    return false;
  }
  // What the control thread does every 10 ms of real time, done here too so
  // virtual time cannot run past frames that should have expired.
  if (video_renderer_) {
    video_renderer_->ExpireFramesWhileHidden(NowMicros());
  }
  if (audio_renderer_) {
    audio_renderer_->PullForStalledDevice(NowMicros());
  }
  // The control thread also publishes size and subtitle changes then: wait
  // for it, or virtual time can run past a rendition or a cue before
  // anyone saw it.
  int width;
  int height;
  if (ShownSizeLocked(&width, &height) &&
      (width != video_width_ || height != video_height_)) {
    return false;
  }
  if (const SubtitleRenderer* renderer = SelectedSubtitleLocked()) {
    const int64_t now = clock_.Now() - LoopOffsetLocked(clock_.Now());
    if (renderer->TextAt(now) != subtitle_text_ ||
        renderer->ImagesKeyAt(now) != subtitle_images_key_) {
      return false;
    }
  }
  // Not clock_.MediaTimeAt(until): an audio-driven clock stops at the end
  // of the audio handed out so far, and would ask for nothing more.
  const int64_t ahead =
      std::llround((until - NowMicros()) * clock_.rate());
  return (!audio_renderer_ || audio_renderer_->HasDataFor(ahead)) &&
         (!video_renderer_ ||
          video_renderer_->HasFramesUntil(clock_.Now() + ahead));
}

bool Player::LiveSettledLocked() const {
  if (seeking_ || live_change_.pending() || track_request_.pending()) {
    return false;
  }
  if (video_renderer_) {
    video_renderer_->ExpireFramesWhileHidden(NowMicros());
  }
  // Everything pushed so far has been decoded, unless a renderer waits for
  // room. Not "has frames past the time asked for": that let time run
  // ahead of a slow decoder until its frames reached just past it, while
  // the vsync then shows the frame covering most of the interval after it,
  // which could still be decoding: under load, frames came a vsync late.
  if (!live_demuxer_->Idle()) {
    return false;
  }
  return (!video_renderer_ || video_queue_.idle() ||
          video_renderer_->Blocked()) &&
         (!audio_renderer_ || audio_queue_.idle() ||
          audio_renderer_->Blocked());
}

void Player::ApplyLiveChange(TrackRequest request, const LiveChange& change) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int stream : {LiveDemuxer::kVideoStream, LiveDemuxer::kAudioStream}) {
      if (!change.track[stream]) {
        continue;
      }
      StreamInfo info = change.info[stream];
      info.index = stream;
      media_info_.streams[stream] = info;
      int* requested = stream == LiveDemuxer::kVideoStream ? &request.video
                                                           : &request.audio;
      *requested = info.type == MediaType::kUnknown ? -1 : stream;
    }
    if (request.video >= 0 && config_.video_frame_types == 0) {
      request.video = -1;
    }
  }
  if (request.pending()) {
    ChangeTracks(request);
  }
  const bool flush[2] = {
      change.flush[0] || change.track[0] || request.rebuild_video,
      change.flush[1] || change.track[1]};
  generation_++;
  if (flush[LiveDemuxer::kVideoStream]) {
    video_queue_.Flush(generation_);
    if (video_renderer_) {
      video_renderer_->Flush(generation_, kNoTimestamp, false);
    }
    if (request.rebuild_video && !change.track[LiveDemuxer::kVideoStream]) {
      live_demuxer_->RestartVideo();
    }
  }
  if (flush[LiveDemuxer::kAudioStream]) {
    audio_queue_.Flush(generation_);
    if (audio_renderer_) {
      audio_renderer_->Flush(generation_, kNoTimestamp);
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (audio_renderer_) {
    audio_renderer_->SetPlaying(true);
  }
  if (flush[LiveDemuxer::kVideoStream]) {
    live_demuxer_->Release(LiveDemuxer::kVideoStream, video_renderer_ != nullptr,
                           change.hold[LiveDemuxer::kVideoStream]);
  }
  if (flush[LiveDemuxer::kAudioStream]) {
    live_demuxer_->Release(LiveDemuxer::kAudioStream, audio_renderer_ != nullptr,
                           change.hold[LiveDemuxer::kAudioStream]);
  }
  // Clears the Interrupt().
  demuxer_->Seek(0);
}

void Player::LoseVideoDecoderForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (video_renderer_) {
    video_renderer_->SimulateDecoderLossForTesting();
  }
}

void Player::HangVideoDecoderForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (video_renderer_) {
    video_renderer_->SimulateDecoderHangForTesting();
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
  if (state == PlayerState::kReady && startup_time_ == 0 &&
      open_time_ != kNoTimestamp) {
    startup_time_ = std::max<int64_t>(MonotonicMicros() - open_time_, 1);
    LOGI("ready %lld ms after the open", Millis(startup_time_));
  }
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
    if (!callback || events.empty()) {
      return;
    }
    dispatching_ = true;
  }
  for (const Event& event : events) {
    callback(user_data, static_cast<int32_t>(event.type), event.arg0,
             event.arg1);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatching_ = false;
  }
  condition_.notify_all();
}

void Player::FailLocked(const std::string& error) {
  error_ = error;
  SetStateLocked(PlayerState::kError);
  PostEventLocked(PlayerEvent::kError);
}

}  // namespace lychee
