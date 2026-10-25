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

std::string ToLower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return text;
}

}  // namespace

Player::Player(const PlayerConfig& config) : config_(config) {}

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
  if (video_renderer_) {
    video_renderer_->Stop();
  }
  if (audio_renderer_) {
    audio_renderer_->Stop();
  }

  std::unique_ptr<VideoRenderer> video;
  std::unique_ptr<AudioRenderer> audio;
  std::unique_ptr<SubtitleRenderer> subtitle;
  std::unique_ptr<Demuxer> demuxer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    video = std::move(video_renderer_);
    audio = std::move(audio_renderer_);
    subtitle = std::move(subtitle_renderer_);
    demuxer = std::move(demuxer_);
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
    track_request_ = TrackRequest();
    input_error_.clear();
    error_.clear();
    media_info_ = MediaInfo();
    video_last_pts_ = audio_last_pts_ = kNoTimestamp;
    video_width_ = video_height_ = 0;
    subtitle_text_.clear();
  }
  video_queue_.Reset(0);
  audio_queue_.Reset(0);
  clock_.Reset(0);
}

void Player::SetEventCallback(EventCallback callback, void* user_data) {
  std::lock_guard<std::mutex> lock(mutex_);
  event_callback_ = callback;
  event_user_data_ = user_data;
}

bool Player::Open(const std::string& url) {
  if (url.empty()) {
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
    if (state_ == PlayerState::kIdle || state_ == PlayerState::kError) {
      return;
    }
    position = std::max<int64_t>(position, 0);
    if (media_info_.duration != kNoTimestamp) {
      position = std::min(position, media_info_.duration);
    }
    seek_pending_ = true;
    seek_target_ = position;
    report_seek_ = true;
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

void Player::SetVideoOutput(void* output) {
  std::shared_ptr<void> retained = RetainVideoOutput(output);
  std::lock_guard<std::mutex> lock(mutex_);
  video_output_ = retained;
  std::lock_guard<std::mutex> display_lock(display_mutex_);
  if (video_for_display_) {
    video_for_display_->SetVideoOutput(std::move(retained));
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
  clock_.SetRate(rate);
  if (audio_renderer_) {
    audio_renderer_->SetRate(rate);
  }
}

void Player::SetLooping(bool looping) {
  std::lock_guard<std::mutex> lock(mutex_);
  looping_ = looping;
}

bool Player::SelectTrack(MediaType type, int track) {
  if (type != MediaType::kVideo && type != MediaType::kAudio &&
      type != MediaType::kSubtitle) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::kIdle || state_ == PlayerState::kError ||
        (!video_renderer_ && !audio_renderer_)) {
      return false;
    }
    int stream = -1;
    if (track >= 0) {
      std::vector<int> streams = TrackStreamsLocked();
      if (track >= static_cast<int>(streams.size())) {
        return false;
      }
      stream = streams[track];
      const StreamInfo& info = media_info_.streams[stream];
      if (info.type != type ||
          (type == MediaType::kSubtitle && !info.subtitle.text)) {
        return false;
      }
    }
    int* requested = type == MediaType::kVideo   ? &track_request_.video
                     : type == MediaType::kAudio ? &track_request_.audio
                                                 : &track_request_.subtitle;
    const int current = type == MediaType::kVideo   ? video_stream_
                        : type == MediaType::kAudio ? audio_stream_
                                                    : subtitle_stream_;
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
    if (!track_request_.pending()) {
      return true;
    }
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
  const int64_t now = clock_.Now();
  if (video_renderer_) {
    stats.video_decoder = video_renderer_->decoder_name();
    stats.hardware = video_renderer_->is_hardware();
    stats.frames_rendered = video_renderer_->frames_rendered();
    stats.frames_dropped = video_renderer_->frames_dropped();
    stats.video_decode_time = video_renderer_->average_decode_time();
    if (video_last_pts_ != kNoTimestamp) {
      stats.video_buffered = std::max<int64_t>(video_last_pts_ - now, 0);
    }
  }
  if (audio_renderer_ && audio_last_pts_ != kNoTimestamp) {
    stats.audio_buffered = std::max<int64_t>(audio_last_pts_ - now, 0);
  }
  return stats;
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

std::string Player::subtitle(int64_t display_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!subtitle_renderer_) {
    return std::string();
  }
  return subtitle_renderer_->TextAt(
      display_time ? clock_.MediaTimeAt(display_time) : clock_.Now());
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
    bool loop = false;
    int64_t seek_target = 0;
    TrackRequest tracks;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (quit_) {
        break;
      }
      if (seek_pending_ || track_request_.pending()) {
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
          int64_t position =
              std::max<int64_t>(clock_.Now() - media_info_.start_time, 0);
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
    if (seek) {
      if (tracks.pending()) {
        ChangeTracks(tracks);
      }
      ProcessSeek(seek_target, loop);
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
      return quit_ || seek_pending_ || track_request_.pending();
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

std::unique_ptr<VideoRenderer> Player::CreateVideoRenderer(
    const StreamInfo& info) {
  auto video =
      std::make_unique<VideoRenderer>(&clock_, config_.video_frame_types);
  std::shared_ptr<void> output;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    output = video_output_;
  }
  std::string error;
  if (!video->Initialize(info.video, config_.hardware_decoding, output,
                         &video_queue_, &error)) {
    LOGW("video stream %d disabled: %s", info.index, error.c_str());
    return nullptr;
  }
  return video;
}

std::unique_ptr<AudioRenderer> Player::CreateAudioRenderer(
    const StreamInfo& info) {
  auto audio = std::make_unique<AudioRenderer>(&clock_);
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
  LOGI("opened %s: format %s, duration %lld us, %zu streams", url.c_str(),
       info.format_name.c_str(), static_cast<long long>(info.duration),
       info.streams.size());

  std::unique_ptr<VideoRenderer> video;
  std::unique_ptr<AudioRenderer> audio;
  std::unique_ptr<SubtitleRenderer> subtitle;
  if (info.default_video_stream >= 0) {
    video = CreateVideoRenderer(info.streams[info.default_video_stream]);
  }
  if (info.default_audio_stream >= 0) {
    audio = CreateAudioRenderer(info.streams[info.default_audio_stream]);
  }
  if (!video && !audio) {
    *error = "no playable audio or video stream";
    return false;
  }
  if (info.default_subtitle_stream >= 0) {
    subtitle =
        CreateSubtitleRenderer(info.streams[info.default_subtitle_stream]);
  }
  clock_.Reset(info.start_time);

  std::lock_guard<std::mutex> lock(mutex_);
  media_info_ = info;
  video_stream_ = video ? info.default_video_stream : -1;
  audio_stream_ = audio ? info.default_audio_stream : -1;
  subtitle_stream_ = subtitle ? info.default_subtitle_stream : -1;
  video_renderer_ = std::move(video);
  audio_renderer_ = std::move(audio);
  subtitle_renderer_ = std::move(subtitle);
  EnableStreamsLocked();
  if (audio_renderer_) {
    audio_renderer_->SetVolume(volume_);
    audio_renderer_->SetRate(rate_);
    audio_renderer_->Start();
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
  if (request.video != kUnchanged) {
    std::unique_ptr<VideoRenderer> old;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      {
        std::lock_guard<std::mutex> display_lock(display_mutex_);
        video_for_display_ = nullptr;
      }
      old = std::move(video_renderer_);
      video_stream_ = -1;
    }
    video_queue_.Abort();
    if (old) {
      old->Stop();
      old.reset();
    }
    video_queue_.Reset(generation_);
    std::unique_ptr<VideoRenderer> video;
    if (request.video >= 0) {
      video = CreateVideoRenderer(media_info_.streams[request.video]);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (video) {
      LOGI("video track -> stream %d", request.video);
      video_renderer_ = std::move(video);
      video_stream_ = request.video;
      video_renderer_->Start();
      PublishVideoLocked();
      const VideoConfig& config = media_info_.streams[video_stream_].video;
      video_width_ = config.display_width();
      video_height_ = config.height;
    } else {
      video_width_ = video_height_ = 0;
    }
    PostEventLocked(PlayerEvent::kVideoSizeChanged, video_width_,
                    video_height_);
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
      old->Stop();
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
      audio_renderer_->Start();
    }
  }
  if (request.subtitle != kUnchanged) {
    std::unique_ptr<SubtitleRenderer> subtitle;
    if (request.subtitle >= 0) {
      subtitle = CreateSubtitleRenderer(media_info_.streams[request.subtitle]);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    LOGI("subtitle track -> stream %d", subtitle ? request.subtitle : -1);
    subtitle_renderer_ = std::move(subtitle);
    subtitle_stream_ = subtitle_renderer_ ? request.subtitle : -1;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  EnableStreamsLocked();
  if (!video_renderer_ && !audio_renderer_) {
    FailLocked("no playable audio or video stream");
  }
  PostEventLocked(PlayerEvent::kTracksChanged);
}

void Player::EnableStreamsLocked() {
  std::vector<int> streams;
  for (int stream : {video_stream_, audio_stream_, subtitle_stream_}) {
    if (stream >= 0) {
      streams.push_back(stream);
    }
  }
  demuxer_->SetEnabledStreams(streams);
}

void Player::PublishVideoLocked() {
  std::lock_guard<std::mutex> lock(display_mutex_);
  video_for_display_ = video_renderer_.get();
  // The output may have changed since the renderer was created.
  if (video_for_display_) {
    video_for_display_->SetVideoOutput(video_output_);
  }
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
  } else if (packet->stream_index == subtitle_stream_ && subtitle_renderer_) {
    subtitle_renderer_->Decode(*packet);
  }
}

void Player::ProcessSeek(int64_t target, bool loop) {
  int64_t media_target = media_info_.start_time + target;
  LOGI("%s to %lld us", loop ? "loop" : "seek", static_cast<long long>(target));
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
  if (subtitle_renderer_) {
    subtitle_renderer_->Flush();
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
    PostEventLocked(PlayerEvent::kSeekCompleted, seek_target_);
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
        SetStateLocked(PlayerState::kBuffering);
      }
      break;
    default:
      break;
  }
  ApplyPlayingLocked();
  UpdateVideoSizeLocked();
  UpdateSubtitleLocked();
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

void Player::UpdateSubtitleLocked() {
  std::string text;
  if (subtitle_renderer_) {
    // Keep the old text until a seek lands.
    if (seek_pending_ || seek_in_progress_ || prerolling_) {
      return;
    }
    text = subtitle_renderer_->TextAt(clock_.Now());
  }
  if (text != subtitle_text_) {
    subtitle_text_ = std::move(text);
    PostEventLocked(PlayerEvent::kSubtitleChanged,
                    static_cast<int64_t>(subtitle_text_.size()));
  }
}

void Player::ApplyPlayingLocked() {
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
  // Not clock_.MediaTimeAt(until): an audio-driven clock stops at the end
  // of the audio handed out so far, and would ask for nothing more.
  const int64_t ahead =
      std::llround((until - NowMicros()) * clock_.rate());
  return (!audio_renderer_ || audio_renderer_->HasDataFor(ahead)) &&
         (!video_renderer_ ||
          video_renderer_->HasFramesUntil(clock_.Now() + ahead));
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
