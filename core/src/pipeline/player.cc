#define LOG_TAG "player"

#include "pipeline/player.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>

#include "base/log.h"
#include "base/thread_name.h"
#include "media/error.h"
#include "media/factory.h"
#include "media/subtitle_file.h"

namespace lychee {

namespace {

// Input with audio or video that no decoder takes has a codec this
// device does not decode; input with neither is not media to play.
std::string NoPlayableStream(const MediaInfo& info) {
  const bool any = std::any_of(info.streams.begin(), info.streams.end(),
                               [](const StreamInfo& stream) {
                                 return stream.type == MediaType::kVideo ||
                                        stream.type == MediaType::kAudio;
                               });
  return TagError(any ? ErrorKind::kCodec : ErrorKind::kFormat,
                  "no playable audio or video stream");
}

// Keep reading until each active stream has this much queued (and after a
// stall, wait for this much before playing again)...
constexpr int64_t kMinQueuedDuration = 3 * kMicrosPerSecond;
// ...unless the queues already hold this many bytes.
constexpr size_t kMaxQueuedBytes = size_t{64} * 1024 * 1024;
// The read thread refills queues that hold enough at most this often, in
// batches; one drained below kReadWakeDuration wakes it at once (media
// plays faster than real time on the virtual clock), and so do seeks and
// track changes.
constexpr auto kReadPoll = std::chrono::milliseconds(100);
constexpr int64_t kReadWakeDuration =
    kMinQueuedDuration - 500 * kMicrosPerMilli;
// The control thread runs the state machine this often, to notice what
// the renderers and the clock do (preroll, the end, starving, subtitle
// cues, a device's fade-out). Paused or ended with nothing in flight
// (IdleLocked()), only a failing decoder or a size change after a paused
// seek is left to notice; calls that need it notify it.
constexpr auto kControlPoll = std::chrono::milliseconds(10);
constexpr auto kIdleControlPoll = std::chrono::milliseconds(100);
// A loop trims at most this much from the container's start and end where
// the tracks start later or end sooner: codec priming and padding (AAC's
// 1024 samples, Opus's pre-skip, a duration rounded up), not content.
constexpr int64_t kMaxLoopTrim = 100 * kMicrosPerMilli;
// Playback logs its stats this often.
constexpr int64_t kStatsLogInterval = 5 * kMicrosPerSecond;
// Input that cannot seek never goes back, so its subtitle cues are
// dropped once they ended this long before the subtitle time playing:
// enough for a later, larger subtitle delay to still find them.
constexpr int64_t kUnseekableSubtitleHistory = 60 * kMicrosPerSecond;

// The video stream automatic variant selection plays: the adaptive one,
// which the demuxer moves between variants; without one (FFmpeg's HLS,
// each variant on its own stream) the highest within |cap| (0: any), or
// the lowest. -1 without variants.
int AutoVariantStream(const std::vector<VariantInfo>& variants, int64_t cap) {
  int best = -1;
  for (size_t i = 0; i < variants.size(); ++i) {
    const VariantInfo& v = variants[i];
    if (v.adaptive) {
      return v.stream;
    }
    if (best < 0) {
      best = static_cast<int>(i);
      continue;
    }
    const VariantInfo& b = variants[best];
    const bool v_fits = cap <= 0 || v.bandwidth <= cap;
    const bool b_fits = cap <= 0 || b.bandwidth <= cap;
    if (v_fits != b_fits ? v_fits
        : v_fits         ? v.bandwidth > b.bandwidth
                         : v.bandwidth < b.bandwidth) {
      best = static_cast<int>(i);
    }
  }
  return best >= 0 ? variants[best].stream : -1;
}

// How far before a seek target an audio decoder must start to output the
// target, after a flush. Opus: 80 ms to converge (RFC 7845's seek
// preroll; Android's decoder drops that much). MP3: a frame's data may
// start up to 511 bytes back in earlier frames (the bit reservoir), and
// each overlaps the one before; until then decoders output silence (40
// to 70 ms of it at 64 kbit/s). 250 ms covers MPEG-1 at 32 kbit/s, the
// lowest bitrate, where 511 bytes span six frames.
int64_t SeekPreroll(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kOpus:
      return 80 * kMicrosPerMilli;
    case AudioCodec::kMP3:
      return 250 * kMicrosPerMilli;
    default:
      return 0;
  }
}

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
    : config_(config), video_device_(config.video_device) {
  // On a decode thread, which holds no lock then; it takes |mutex_|, so no
  // thread may wait for a decode thread while it holds that. Taking it
  // means the read thread has either yet to check the queues or is
  // waiting, so the notification is not lost.
  auto wake_read_thread = [this] {
    {
      std::lock_guard<std::mutex> lock(mutex_);
    }
    condition_.notify_all();
  };
  video_queue_.SetLowWater(kReadWakeDuration, wake_read_thread);
  audio_queue_.SetLowWater(kReadWakeDuration, wake_read_thread);
}

Player::~Player() {
  ClearNext();
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  StopPipeline();
}

void Player::StopPipeline() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    for (DataSource* source : subtitle_sources_) {
      source->Interrupt();
    }
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
  // |quit_| keeps AddSubtitle() from starting more meanwhile.
  std::vector<std::thread> subtitle_loads;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_loads.swap(subtitle_loads_);
  }
  for (std::thread& load : subtitle_loads) {
    load.join();
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
  std::map<int, ExternalSubtitle> external_subtitles;
  std::unique_ptr<Demuxer> demuxer;
  // A joined item is dropped, even by SwitchSource(): its demuxer was
  // aborted with the read.
  std::unique_ptr<Join> join;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    video = std::move(video_renderer_);
    audio = std::move(audio_renderer_);
    subtitles = std::move(subtitle_renderers_);
    subtitle_renderers_.clear();
    external_subtitles = std::move(external_subtitles_);
    external_subtitles_.clear();
    histories_.clear();
    demuxer = std::move(demuxer_);
    join = std::move(join_);
    item_base_ = 0;
    waiting_for_next_ = false;
    advance_pending_ = false;
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
    PublishLoopMapLocked();
    track_request_ = TrackRequest();
    input_error_.clear();
    error_.clear();
    error_code_ = ErrorCode();
    media_info_ = MediaInfo();
    variants_.clear();
    selected_variant_ = -1;
    video_last_pts_ = audio_last_pts_ = kNoTimestamp;
    video_width_ = video_height_ = video_rotation_ = 0;
    switch_seek_ = false;
    continue_shown_ = kNoTimestamp;
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
  variant_switches_.clear();
  variant_logged_ = -1;
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
  LOGE("open or close from an event callback: call it from another thread");
  return true;
}

bool Player::Open(const std::string& url, const std::string& headers) {
  if (url.empty() || OnControlThread()) {
    return false;
  }
  ClearNext();
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  StopPipeline();
  std::lock_guard<std::mutex> lock(mutex_);
  SetStateLocked(PlayerState::kLoading);
  demuxer_ = CreateDemuxer(headers);
  if (demuxer_) {
    // An HLS master playlist starts within the cap.
    demuxer_->SetVariantPolicy(-1, max_bitrate_);
  }
  thread_ = std::thread(&Player::Run, this, url);
  return true;
}

bool Player::Close() {
  if (OnControlThread()) {
    return false;
  }
  ClearNext();
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  bool was_open;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_open = state_ != PlayerState::kIdle;
  }
  StopPipeline();
  if (!was_open) {
    return true;
  }
  LOGI("closed");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    PostEventLocked(PlayerEvent::kStateChanged,
                    static_cast<int64_t>(PlayerState::kIdle));
  }
  // No control thread is left to hand it out. As there, an open or close
  // from inside the callback is refused rather than deadlocking on
  // |open_mutex_|.
  const Player* control_thread_player = t_control_thread_player;
  t_control_thread_player = this;
  DispatchEvents();
  t_control_thread_player = control_thread_player;
  return true;
}

bool Player::SwitchSource(const std::string& url,
                          bool keep_position,
                          const std::string& headers) {
  if (url.empty() || OnControlThread()) {
    return false;
  }
  std::lock_guard<std::mutex> open_lock(open_mutex_);
  bool keep;
  int64_t position = 0;
  int64_t shown_end = kNoTimestamp;
  int64_t duration;
  int width;
  int height;
  int rotation;
  std::map<int, ExternalSubtitle> subtitles;
  int subtitle_stream;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    keep = !live_ && state_ != PlayerState::kIdle;
    if (keep_position && keep) {
      position = PositionLocked();
      if (video_renderer_ && !seek_pending_ && !seek_in_progress_) {
        const int64_t end = video_renderer_->shown_end();
        if (end != kNoTimestamp) {
          shown_end = end - LoopOffsetLocked(end) - media_info_.start_time;
        }
      }
    }
    duration = media_info_.duration;
    width = video_width_;
    height = video_height_;
    rotation = video_rotation_;
    // The same content: subtitle files added for it still apply.
    subtitles.swap(external_subtitles_);
    subtitle_stream = subtitle_stream_;
  }
  StopPipeline();
  std::lock_guard<std::mutex> lock(mutex_);
  if (keep) {
    LOGI("switching source at %lld ms to %s", Millis(position), url.c_str());
    external_subtitles_ = std::move(subtitles);
    if (external_subtitles_.count(subtitle_stream)) {
      subtitle_stream_ = subtitle_stream;
    }
    // What the caller shows stays until the new input reports its own.
    media_info_.duration = duration;
    video_width_ = width;
    video_height_ = height;
    video_rotation_ = rotation;
    if (position > 0) {
      seek_pending_ = true;
      seek_target_ = position;
      switch_seek_ = true;
      continue_shown_ = shown_end;
    }
    SetStateLocked(PlayerState::kBuffering);
  } else {
    SetStateLocked(PlayerState::kLoading);
  }
  demuxer_ = CreateDemuxer(headers);
  if (demuxer_) {
    demuxer_->SetVariantPolicy(-1, max_bitrate_);
  }
  thread_ = std::thread(&Player::Run, this, url);
  return true;
}

bool Player::SetNext(const std::string& url, const std::string& headers) {
  ClearNext();
  if (url.empty()) {
    return true;
  }
  auto item = std::make_unique<NextItem>();
  item->url = url;
  item->demuxer = CreateDemuxer(headers);
  if (!item->demuxer) {
    return false;
  }
  std::unique_ptr<NextItem> replaced;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Another SetNext() since the ClearNext() above.
    replaced = std::move(next_);
    // An HLS master playlist starts within the cap, as at an open.
    item->demuxer->SetVariantPolicy(-1, max_bitrate_);
    item->serial = ++next_serial_;
    NextItem* next = item.get();
    // Started under the lock, so that whoever takes |next_| joins it.
    next->thread = std::thread([this, next] {
      SetCurrentThreadName("lychee-next");
      std::string error;
      const bool ok = next->demuxer->Open(next->url, &error);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        next->done = true;
        next->ok = ok;
        next->error = error;
      }
      condition_.notify_all();
    });
    next_ = std::move(item);
    LOGI("opening the next item %s", url.c_str());
  }
  DropNext(std::move(replaced));
  return true;
}

void Player::ClearNext() {
  std::unique_ptr<NextItem> item;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++next_serial_;
    item = std::move(next_);
    // One the read thread joined, but not yet playing, goes too: a resync
    // flushes what was read of it and reads this item's end again.
    if (join_ && clock_.Now() < join_->at) {
      track_request_.resync = true;
      demuxer_->Interrupt();
    }
  }
  condition_.notify_all();
  DropNext(std::move(item));
}

void Player::DropNext(std::unique_ptr<NextItem> item) {
  if (!item) {
    return;
  }
  // Ends an open in progress.
  item->demuxer->Abort();
  if (item->thread.joinable()) {
    item->thread.join();
  }
}

bool Player::ContinuesLocked(const MediaInfo& next,
                             int* video_stream,
                             int* audio_stream,
                             std::string* why) const {
  // Seeks before it starts take it back, to read it from its start again.
  if (!next.seekable) {
    *why = "it cannot seek";
    return false;
  }
  const bool video = video_stream_ >= 0;
  const bool next_video =
      next.default_video_stream >= 0 && config_.video_frame_types != 0;
  const bool audio = audio_stream_ >= 0;
  const bool next_audio = next.default_audio_stream >= 0;
  if (video != next_video || audio != next_audio) {
    *why = "other kinds of tracks";
    return false;
  }
  // The decoders go on after a drain and a flush, as at a loop's boundary,
  // so the next item must need no other configuration.
  if (video) {
    const VideoConfig& a = media_info_.streams[video_stream_].video;
    const VideoConfig& b = next.streams[next.default_video_stream].video;
    if (a.codec != b.codec || a.width != b.width || a.height != b.height ||
        a.rotation != b.rotation || a.extradata != b.extradata) {
      *why = "another video format";
      return false;
    }
  }
  if (audio) {
    const AudioConfig& a = media_info_.streams[audio_stream_].audio;
    const AudioConfig& b = next.streams[next.default_audio_stream].audio;
    if (a.codec != b.codec || a.sample_rate != b.sample_rate ||
        a.channels != b.channels || a.extradata != b.extradata) {
      *why = "another audio format";
      return false;
    }
  }
  *video_stream = video ? next.default_video_stream : -1;
  *audio_stream = audio ? next.default_audio_stream : -1;
  return true;
}

bool Player::JoinNext() {
  std::unique_ptr<NextItem> item;
  int video_stream = -1;
  int audio_stream = -1;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Looping plays this item again instead; one looped already has its
    // renderers set to end with its pass (ApplyLoopEndLocked()). This item
    // must seek for a seek before the next one starts to take it back.
    if (!next_ || !next_->done || !next_->ok || join_ || looping_ || live_ ||
        loops_ != 0 || !media_info_.seekable ||
        media_info_.duration == kNoTimestamp || media_info_.duration <= 0 ||
        seek_pending_ || track_request_.pending() || quit_) {
      return false;
    }
    std::string why;
    if (!ContinuesLocked(next_->demuxer->media_info(), &video_stream,
                         &audio_stream, &why)) {
      LOGI("the next item %s switches at the end: %s", next_->url.c_str(),
           why.c_str());
      return false;
    }
    item = std::move(next_);
  }
  // It has opened (or came back from a join, its thread joined).
  if (item->thread.joinable()) {
    item->thread.join();
  }
  // This item ends where its tracks do, as a loop's pass would.
  SetLoopSpan();
  auto join = std::make_unique<Join>();
  join->info = item->demuxer->media_info();
  join->video_stream = video_stream;
  join->audio_stream = audio_stream;
  join->at = item_base_ + loop_start_ + loop_length_;
  join->base = join->at - join->info.start_time;
  std::vector<int> streams;
  for (int stream : {video_stream, audio_stream}) {
    if (stream >= 0) {
      streams.push_back(stream);
    }
  }
  for (const StreamInfo& stream : join->info.streams) {
    if (stream.type == MediaType::kSubtitle) {
      if (auto subtitle = CreateSubtitleRenderer(stream)) {
        join->subtitles[stream.index] = std::move(subtitle);
        streams.push_back(stream.index);
      }
    }
  }
  item->demuxer->SetEnabledStreams(streams);
  if (video_stream_ >= 0) {
    video_queue_.Push(EncodedPacket::CreateBoundary(video_stream_, join->at));
  }
  if (audio_stream_ >= 0) {
    audio_queue_.Push(EncodedPacket::CreateBoundary(audio_stream_, join->at));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (quit_) {
    return false;
  }
  LOGI("the next item %s joins at %lld ms", item->url.c_str(),
       Millis(join->at - media_info_.start_time - item_base_));
  video_last_pts_ = std::max(video_last_pts_, join->at);
  audio_last_pts_ = std::max(audio_last_pts_, join->at);
  join->previous = std::move(demuxer_);
  demuxer_ = std::move(item->demuxer);
  join->item = std::move(item);
  join_ = std::move(join);
  // From here on, of the joined item's streams.
  spans_.clear();
  PublishLoopMapLocked();
  return true;
}

std::unique_ptr<Demuxer> Player::CrossLocked() {
  std::unique_ptr<Join> join = std::move(join_);
  LOGI("the next item %s plays", join->item->url.c_str());
  media_info_ = std::move(join->info);
  video_stream_ = join->video_stream;
  audio_stream_ = join->audio_stream;
  subtitle_renderers_ = std::move(join->subtitles);
  // Subtitle files added were for the item before.
  external_subtitles_.clear();
  subtitle_stream_ =
      subtitle_renderers_.count(media_info_.default_subtitle_stream)
          ? media_info_.default_subtitle_stream
          : -1;
  item_base_ = join->base;
  loops_ = 0;
  loop_start_ = loop_length_ = 0;
  loop_end_ = kNoTimestamp;
  PublishLoopMapLocked();
  variant_switches_.clear();
  PostEventLocked(PlayerEvent::kItemChanged, 1);
  PostEventLocked(PlayerEvent::kMediaInfo, media_info_.duration == kNoTimestamp
                                               ? -1
                                               : media_info_.duration);
  PostEventLocked(PlayerEvent::kTracksChanged);
  // The size goes on (ContinuesLocked()), but for the pixel aspect ratio.
  if (video_stream_ >= 0) {
    const VideoConfig& config = media_info_.streams[video_stream_].video;
    if (config.display_width() != video_width_ ||
        config.height != video_height_) {
      video_width_ = config.display_width();
      video_height_ = config.height;
      PostEventLocked(PlayerEvent::kVideoSizeChanged, video_width_,
                      video_height_);
    }
  }
  // The read thread waits for this (ShouldReadLocked()).
  condition_.notify_all();
  return std::move(join->previous);
}

std::unique_ptr<Player::NextItem> Player::UnjoinLocked() {
  std::unique_ptr<Join> join = std::move(join_);
  std::unique_ptr<NextItem> item = std::move(join->item);
  LOGI("taking back the next item %s", item->url.c_str());
  item->demuxer = std::move(demuxer_);
  demuxer_ = std::move(join->previous);
  spans_.clear();
  PublishLoopMapLocked();
  return item;
}

void Player::RestoreNext(std::unique_ptr<NextItem> item) {
  // Read from its start again, and out of a Seek()'s interrupt.
  if (!item->demuxer->Seek(item->demuxer->media_info().start_time)) {
    LOGW("the next item %s cannot go back to its start: dropped",
         item->url.c_str());
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Unless SetNext() or ClearNext() came since.
  if (!next_ && item->serial == next_serial_) {
    LOGI("the next item %s joins at the end again", item->url.c_str());
    next_ = std::move(item);
  }
}

void Player::AdvanceToNext() {
  std::unique_ptr<NextItem> item;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Replaced or cleared since the end: the state machine sees to it.
    if (!next_ || !next_->done) {
      return;
    }
    item = std::move(next_);
  }
  if (item->thread.joinable()) {
    item->thread.join();
  }
  if (!item->ok) {
    std::lock_guard<std::mutex> lock(mutex_);
    LOGE("open %s failed: %s", item->url.c_str(), item->error.c_str());
    PostEventLocked(PlayerEvent::kItemChanged, 0);
    FailLocked(item->error);
    return;
  }
  const MediaInfo info = item->demuxer->media_info();
  LOGI("switching to the next item %s: format %s, duration %lld us",
       item->url.c_str(), info.format_name.c_str(),
       static_cast<long long>(info.duration));
  std::map<int, std::unique_ptr<SubtitleRenderer>> subtitles;
  std::map<int, PacketHistory> histories;
  for (const StreamInfo& stream : info.streams) {
    if (stream.type == MediaType::kSubtitle) {
      if (auto subtitle = CreateSubtitleRenderer(stream)) {
        subtitles[stream.index] = std::move(subtitle);
      }
    } else if (!info.seekable && (stream.type == MediaType::kAudio ||
                                  (stream.type == MediaType::kVideo &&
                                   config_.video_frame_types))) {
      histories.try_emplace(stream.index, kMaxQueuedBytes,
                            stream.type == MediaType::kAudio);
    }
  }
  // Swapped with the item's streams in one go: callers look the renderers'
  // streams up in |media_info_|.
  std::unique_ptr<Demuxer> previous;
  VideoRenderer::Ptr old_video;
  std::unique_ptr<AudioRenderer> old_audio;
  std::map<int, std::unique_ptr<SubtitleRenderer>> old_subtitles;
  std::map<int, ExternalSubtitle> old_external_subtitles;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    {
      std::lock_guard<std::mutex> display_lock(display_mutex_);
      video_for_display_ = nullptr;
    }
    old_video = std::move(video_renderer_);
    if (old_video) {
      const VideoRenderer::Counts counts = old_video->counts();
      retired_video_.rendered += counts.rendered;
      retired_video_.dropped += counts.dropped;
      retired_video_.decoded += counts.decoded;
    }
    old_audio = std::move(audio_renderer_);
    video_stream_ = audio_stream_ = -1;
    previous = std::move(demuxer_);
    demuxer_ = std::move(item->demuxer);
    media_info_ = info;
    histories_ = std::move(histories);
    old_subtitles = std::move(subtitle_renderers_);
    subtitle_renderers_ = std::move(subtitles);
    old_external_subtitles = std::move(external_subtitles_);
    external_subtitles_.clear();
    subtitle_stream_ = subtitle_renderers_.count(info.default_subtitle_stream)
                           ? info.default_subtitle_stream
                           : -1;
    item_base_ = 0;
    loops_ = 0;
    loop_start_ = loop_length_ = 0;
    loop_end_ = kNoTimestamp;
    spans_.clear();
    PublishLoopMapLocked();
    // Reported as at its start until it plays, as a seek is.
    seek_target_ = 0;
    seek_in_progress_ = true;
    current_seek_reported_ = false;
    PostEventLocked(PlayerEvent::kItemChanged, 0);
    PostEventLocked(PlayerEvent::kMediaInfo,
                    info.duration == kNoTimestamp ? -1 : info.duration);
  }
  video_queue_.Abort();
  audio_queue_.Abort();
  if (old_video) {
    old_video->Stop();
  }
  if (old_audio) {
    old_audio->Retire();
    if (old_audio->SinkStopped()) {
      old_audio->Stop();
    } else {
      retired_audio_.push_back(std::move(old_audio));
    }
  }
  TrackRequest request;
  request.video = info.default_video_stream >= 0 && config_.video_frame_types
                      ? info.default_video_stream
                      : -1;
  request.audio = info.default_audio_stream;
  request.new_item = true;
  ChangeTracks(request);
  ProcessSeek(0, false, false, 0, true);
}

bool Player::OpenLive(int64_t latency) {
  if (OnControlThread()) {
    return false;
  }
  ClearNext();
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
    if (LiveWindow window; LiveWindowLocked(&window)) {
      position = std::clamp(position, window.start, window.end);
    }
    seek_pending_ = true;
    seek_target_ = position;
    report_seek_ = true;
    switch_seek_ = false;
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

void Player::Fade(float volume, int64_t duration) {
  std::lock_guard<std::mutex> lock(mutex_);
  volume_ = volume;
  if (audio_renderer_) {
    audio_renderer_->Fade(volume, duration);
  }
}

void Player::SetGain(float gain_db, float peak) {
  if (std::isnan(gain_db)) {
    gain_db = 0;
  }
  if (std::isnan(peak)) {
    peak = 0;
  }
  const float db = std::clamp(gain_db, kMinGainDb, kMaxGainDb);
  std::lock_guard<std::mutex> lock(mutex_);
  gain_ = std::pow(10.0f, db / 20.0f);
  gain_peak_ = peak;
  if (audio_renderer_) {
    audio_renderer_->SetGain(gain_, gain_peak_);
  }
}

bool Player::GetPlayedSamples(int64_t time, float* out, int frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!audio_renderer_) {
    return false;
  }
  audio_renderer_->GetPlayedSamples(time, out, frames);
  return true;
}

void Player::SetSubtitleDelay(int64_t delay) {
  std::lock_guard<std::mutex> lock(mutex_);
  subtitle_delay_ = delay;
  UpdateSubtitleLocked();
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
    // renderer, from the request this makes.
    PublishVideoLocked();
    CheckVideoRendererLocked();
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
      const MediaType stream_type = stream >= kExternalSubtitleStream
                                        ? MediaType::kSubtitle
                                        : media_info_.streams[stream].type;
      if (stream_type != type) {
        return false;
      }
    }
    if (type == MediaType::kSubtitle) {
      // Its stream is read all along: nothing to seek or rebuild.
      if (stream >= 0 && !subtitle_renderers_.count(stream) &&
          !external_subtitles_.count(stream)) {
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

bool Player::SelectVariant(int index) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == PlayerState::kIdle || state_ == PlayerState::kError ||
        live_ || (!video_renderer_ && !audio_renderer_) || index < -1 ||
        index >= static_cast<int>(variants_.size()) ||
        (index >= 0 && config_.video_frame_types == 0)) {
      return false;
    }
    LOGI("variant %d selected", index);
    selected_variant_ = index;
    ApplyVariantsLocked();
  }
  condition_.notify_all();
  return true;
}

void Player::SetMaxBitrate(int64_t bitrate) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    max_bitrate_ = std::max<int64_t>(bitrate, 0);
    ApplyVariantsLocked();
  }
  condition_.notify_all();
}

void Player::ApplyVariantsLocked() {
  if (!demuxer_) {
    return;
  }
  const bool pinned = selected_variant_ >= 0 &&
                      selected_variant_ < static_cast<int>(variants_.size());
  demuxer_->SetVariantPolicy(
      pinned && variants_[selected_variant_].adaptive ? selected_variant_ : -1,
      max_bitrate_);
  if (variants_.empty() || live_ || state_ == PlayerState::kError ||
      (!video_renderer_ && !audio_renderer_)) {
    return;
  }
  const int want = pinned ? variants_[selected_variant_].stream
                          : AutoVariantStream(variants_, max_bitrate_);
  const int current =
      track_request_.video != kUnchanged ? track_request_.video : video_stream_;
  // Video turned off stays off.
  if (want < 0 || current < 0 || want == current) {
    return;
  }
  LOGI("variant: video track -> stream %d", want);
  track_request_.video = want == video_stream_ ? kUnchanged : want;
  track_request_.resync = true;
  demuxer_->Interrupt();
}

std::vector<VariantInfo> Player::variants() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return variants_;
}

int Player::playing_variant() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return PlayingVariantLocked();
}

int Player::selected_variant() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selected_variant_;
}

bool Player::LiveWindowLocked(LiveWindow* window) const {
  if (!demuxer_ || live_ || !media_info_.seekable ||
      state_ == PlayerState::kError || (!video_renderer_ && !audio_renderer_) ||
      !demuxer_->live_window(window)) {
    return false;
  }
  if (window->moving) {
    // Moved on since the playlist was read, which only happens when the
    // read needs segments.
    const int64_t elapsed =
        std::max<int64_t>(MonotonicMicros() - window->updated, 0);
    window->end += elapsed;
    window->edge += elapsed;
    if (window->sliding) {
      window->start = std::min(window->start + elapsed, window->edge);
    }
  }
  return true;
}

bool Player::seekable_range(int64_t* start, int64_t* end) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!media_info_.seekable || live_ || state_ == PlayerState::kError ||
      (!video_renderer_ && !audio_renderer_)) {
    return false;
  }
  if (LiveWindow window; LiveWindowLocked(&window)) {
    *start = window.start;
    *end = window.end;
    return true;
  }
  *start = 0;
  *end = media_info_.duration;
  return true;
}

int64_t Player::live_edge() const {
  std::lock_guard<std::mutex> lock(mutex_);
  LiveWindow window;
  return LiveWindowLocked(&window) ? window.edge : kNoTimestamp;
}

int Player::AddSubtitle(const std::string& url,
                        const std::string& language,
                        const std::string& title,
                        const std::string& encoding,
                        const std::string& headers,
                        bool select) {
  if (url.empty()) {
    return -1;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (quit_ || live_ || state_ == PlayerState::kIdle ||
      state_ == PlayerState::kError) {
    return -1;
  }
  const int stream = next_external_subtitle_++;
  subtitle_loads_pending_++;
  subtitle_loads_.emplace_back(&Player::LoadSubtitle, this, stream, url,
                               language, title, encoding, headers, select);
  return stream;
}

void Player::LoadSubtitle(int stream,
                          const std::string& url,
                          std::string language,
                          std::string title,
                          const std::string& encoding,
                          const std::string& headers,
                          bool select) {
  SetCurrentThreadName("lychee-subtitle");
  std::string error;
  std::string data;
  std::unique_ptr<DataSource> source = OpenInput(url, &error, headers);
  bool stopped = false;
  if (source) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped = quit_;
      if (!stopped) {
        subtitle_sources_.insert(source.get());
      }
    }
    // The whole file: subtitles are small, and once in memory a seek
    // needs nothing more.
    uint8_t buffer[64 * 1024];
    while (!stopped) {
      const int64_t read = source->Read(static_cast<int64_t>(data.size()),
                                        buffer, sizeof(buffer));
      if (read < 0) {
        error = source->error();
        break;
      }
      if (read == 0) {
        break;
      }
      data.append(reinterpret_cast<const char*>(buffer),
                  static_cast<size_t>(read));
      if (data.size() > kMaxSubtitleFileBytes) {
        error = "larger than " + std::to_string(kMaxSubtitleFileBytes >> 20) +
                " MB";
        break;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    subtitle_sources_.erase(source.get());
  }
  std::vector<SubtitleCue> cues;
  std::string format;
  if (error.empty() && !ParseSubtitleFile(data, encoding, &cues, &format)) {
    error = "not a SubRip, WebVTT or ASS file with cues";
  }
  ExternalSubtitle subtitle;
  if (error.empty()) {
    subtitle.renderer = std::make_unique<SubtitleRenderer>();
    for (SubtitleCue& cue : cues) {
      subtitle.renderer->AddCue(std::move(cue));
    }
    subtitle.codec = format;
    subtitle.language = std::move(language);
    subtitle.title = std::move(title);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  subtitle_loads_pending_--;
  if (quit_) {
    return;
  }
  if (!error.empty()) {
    LOGW("subtitle %s not added: %s", url.c_str(), error.c_str());
    PostEventLocked(PlayerEvent::kSubtitleAdded, stream, 0);
    return;
  }
  LOGI("subtitle %s added: %s, %zu cues, stream %d", url.c_str(),
       subtitle.codec.c_str(), cues.size(), stream);
  external_subtitles_[stream] = std::move(subtitle);
  if (select) {
    subtitle_stream_ = stream;
  }
  PostEventLocked(PlayerEvent::kSubtitleAdded, stream, 1);
  PostEventLocked(PlayerEvent::kTracksChanged);
  UpdateSubtitleLocked();
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
  return PositionLocked();
}

int64_t Player::PositionLocked() const {
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
      now - ItemOffsetLocked(now) - media_info_.start_time, 0);
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
  // Read into the next pass or item: buffered to the end of this one.
  end -= ItemOffsetLocked(now) + media_info_.start_time;
  if (media_info_.duration != kNoTimestamp) {
    end = std::min(end, media_info_.duration);
  }
  return std::max<int64_t>(end, 0);
}

bool Player::video_size(int* width, int* height) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Not whether there is a renderer: a source switch keeps the size of
  // the video on screen while the new input loads.
  if (video_width_ <= 0 || video_height_ <= 0) {
    return false;
  }
  *width = video_width_;
  *height = video_height_;
  return true;
}

int Player::video_rotation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return video_rotation_;
}

std::string Player::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

ErrorCode Player::error_code() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_code_;
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
    stats.video_offset = video_renderer_->average_offset();
    if (const int variant = PlayingVariantLocked(); variant >= 0) {
      stats.variant_bandwidth = variants_[variant].bandwidth;
    }
    if (video_last_pts_ != kNoTimestamp) {
      stats.video_buffered = std::max<int64_t>(video_last_pts_ - now, 0);
    }
  }
  if (audio_renderer_) {
    stats.audio_decoder = audio_renderer_->decoder_name();
    stats.audio_output_sample_rate =
        audio_renderer_->output_format().sample_rate;
    stats.audio_output_channels = audio_renderer_->output_format().channels;
    stats.audio_output_latency = audio_renderer_->output_latency();
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
          "video %s%s %.1f fps, %lld dropped, decode %.1f ms, offset %.1f "
          "ms, read %lld kbit/s; audio %s read %lld kbit/s, latency %lld ms; "
          "buffered video %lld ms, audio %lld ms; download %lld kbit/s, "
          "variant %lld kbit/s",
          stats.video_decoder, stats.hardware ? " hw" : "", fps,
          static_cast<long long>(stats.frames_dropped - last.frames_dropped),
          static_cast<double>(stats.video_decode_time) / kMicrosPerMilli,
          static_cast<double>(stats.video_offset) / kMicrosPerMilli, video_kbps,
          stats.audio_decoder, audio_kbps, Millis(stats.audio_output_latency),
          Millis(stats.video_buffered), Millis(stats.audio_buffered),
          static_cast<long long>(
              stats.download_rate >= 0 ? stats.download_rate / 1000 : -1),
          static_cast<long long>(stats.variant_bandwidth / 1000));
    }
  }
  stats_logged_ = stats;
  stats_log_at_ = now + kStatsLogInterval;
}

int Player::PlayingVariantLocked() const {
  if (variant_switches_.empty()) {
    return -1;
  }
  const int64_t now = clock_.Now();
  int variant = variant_switches_.front().second;
  for (const auto& [pts, switched_to] : variant_switches_) {
    if (pts > now) {
      break;
    }
    variant = switched_to;
  }
  return variant < static_cast<int>(variants_.size()) ? variant : -1;
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
  for (const auto& [stream, subtitle] : external_subtitles_) {
    streams.push_back(stream);
  }
  return streams;
}

std::vector<TrackInfo> Player::tracks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<TrackInfo> tracks;
  for (int index : TrackStreamsLocked()) {
    if (auto it = external_subtitles_.find(index);
        it != external_subtitles_.end()) {
      TrackInfo track;
      track.type = MediaType::kSubtitle;
      track.stream_index = index;
      track.selected = index == subtitle_stream_;
      track.codec = it->second.codec;
      track.language = it->second.language;
      track.title = it->second.title;
      track.external = true;
      tracks.push_back(std::move(track));
      continue;
    }
    const StreamInfo& info = media_info_.streams[index];
    TrackInfo track;
    track.type = info.type;
    track.stream_index = info.index;
    track.selected = index == video_stream_ || index == audio_stream_ ||
                     index == subtitle_stream_;
    track.language = info.language;
    track.title = info.title;
    track.is_default = info.default_track;
    track.forced = info.forced;
    track.role = info.role;
    track.bitrate = info.bitrate;
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
  if (auto it = external_subtitles_.find(subtitle_stream_);
      it != external_subtitles_.end()) {
    return it->second.renderer.get();
  }
  auto it = subtitle_renderers_.find(subtitle_stream_);
  return it == subtitle_renderers_.end() ? nullptr : it->second.get();
}

int64_t Player::SubtitleTimeLocked(int64_t media_time) const {
  // Subtitle packets keep the file's timeline, so every pass finds the
  // same cues.
  int64_t time = media_time - LoopOffsetLocked(media_time) - subtitle_delay_;
  if (subtitle_stream_ >= kExternalSubtitleStream) {
    time -= media_info_.start_time;
  }
  return time;
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
  return renderer->ImagesAt(SubtitleTimeLocked(media_time), width, height,
                            video_width_, video_height_);
}

std::string Player::subtitle(int64_t display_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  SubtitleRenderer* renderer = SelectedSubtitleLocked();
  if (!renderer) {
    return std::string();
  }
  const int64_t media_time =
      display_time ? clock_.MediaTimeAt(display_time) : clock_.Now();
  return renderer->TextAt(SubtitleTimeLocked(media_time));
}

bool Player::HasNewFrame(int64_t display_time) const {
  std::lock_guard<std::mutex> lock(display_mutex_);
  return video_for_display_ && video_for_display_->HasNewFrame(display_time);
}

std::shared_ptr<VideoFrame> Player::AcquireFrame(int64_t display_time) {
  // Pulls from several threads present in the order they took frames.
  std::lock_guard<std::mutex> present_lock(present_mutex_);
  std::function<void(int64_t)> present;
  std::shared_ptr<VideoFrame> frame;
  {
    std::lock_guard<std::mutex> lock(display_mutex_);
    if (video_for_display_) {
      frame = video_for_display_->Acquire(display_time, &present);
    }
  }
  // Outside the display lock, which the control thread takes under
  // |mutex_|. The present holds what it releases.
  if (present) {
    present(display_time);
  }
  return frame;
}

int64_t Player::UnloopedPts(int64_t pts) const {
  if (pts == kNoTimestamp) {
    return pts;
  }
  std::lock_guard<std::mutex> lock(loop_mutex_);
  return pts - loop_map_.Offset(pts);
}

int64_t Player::LoopMap::Offset(int64_t media_time) const {
  // A joined item's single pass.
  if (join_at != kNoTimestamp && media_time >= join_at) {
    return join_base;
  }
  if (loops == 0) {
    return base;
  }
  // Still the last pass when it ends there.
  if (end != kNoTimestamp) {
    media_time = std::min(media_time, end - 1);
  }
  // Pass 0 runs from the file's start to |start| + |length|.
  const int64_t pass =
      std::clamp<int64_t>((media_time - base - start) / length, 0, loops);
  return base + pass * length;
}

Player::LoopMap Player::LoopMapLocked() const {
  LoopMap map{item_base_, loops_, loop_start_, loop_length_, loop_end_};
  if (join_) {
    map.join_at = join_->at;
    map.join_base = join_->base;
  }
  return map;
}

int64_t Player::LoopOffsetLocked(int64_t media_time) const {
  return LoopMapLocked().Offset(media_time);
}

int64_t Player::ItemOffsetLocked(int64_t media_time) const {
  // The item before a joined one has a single pass (JoinNext()).
  return join_ ? item_base_ : LoopOffsetLocked(media_time);
}

void Player::PublishLoopMapLocked() {
  const LoopMap map = LoopMapLocked();
  std::lock_guard<std::mutex> lock(loop_mutex_);
  loop_map_ = map;
}

void Player::ApplyLoopEndLocked() {
  // A seek resets |loops_| and flushes the renderers.
  if (seeking_ || seek_pending_) {
    return;
  }
  loop_end_ = kNoTimestamp;
  int64_t end = kNoTimestamp;
  if (!looping_ && loops_ > 0) {
    const int64_t pass =
        (LoopOffsetLocked(clock_.Now()) - item_base_) / loop_length_;
    if (loops_ > pass) {
      end = item_base_ + loop_start_ + (pass + 1) * loop_length_;
    }
  }
  loop_end_ = end;
  PublishLoopMapLocked();
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
    bool advance = false;
    std::unique_ptr<Demuxer> crossed;
    std::unique_ptr<NextItem> unjoined;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (quit_) {
        break;
      }
      // The joined item plays from when the clock reaches it.
      if (join_ && !reading_ && clock_.Now() >= join_->at) {
        crossed = CrossLocked();
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
        // Seek() interrupted any read in flight; wait for it to return.
        seeking_ = true;
        condition_.wait(lock, [this] { return !reading_ || quit_; });
        if (quit_) {
          break;
        }
        // The joined item, unless it plays already, waits for the end
        // again.
        if (join_) {
          if (clock_.Now() >= join_->at) {
            crossed = CrossLocked();
          } else {
            unjoined = UnjoinLocked();
          }
        }
        advance_pending_ = false;
        tracks = track_request_;
        track_request_ = TrackRequest();
        if (seek_pending_) {
          seek_pending_ = false;
          loop = loop_seek_pending_;
          loop_seek_pending_ = false;
          current_seek_reported_ = report_seek_;
          report_seek_ = false;
          // One requested while loading (a start position) is clamped only
          // now that the duration is known.
          if (media_info_.duration != kNoTimestamp) {
            seek_target_ = std::min(seek_target_, media_info_.duration);
          }
          // A source switch goes on from the frame on screen. Input that
          // cannot seek ignores it as any seek, and starts where it starts.
          if (switch_seek_) {
            switch_seek_ = false;
            resync = media_info_.seekable;
          }
        } else if (!seek_in_progress_) {
          // A track change resumes where playback is.
          resync = true;
          const int64_t now = clock_.Now();
          const int64_t offset = LoopOffsetLocked(now);
          const int64_t passes = offset - item_base_;
          resync_loops = passes ? passes / loop_length_ : 0;
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
      } else if (advance_pending_) {
        advance_pending_ = false;
        advance = true;
        seeking_ = true;
        demuxer_->Interrupt();
        condition_.wait(lock, [this] { return !reading_ || quit_; });
        if (quit_) {
          break;
        }
      }
    }
    crossed.reset();
    if (unjoined) {
      RestoreNext(std::move(unjoined));
    }
    if (advance) {
      AdvanceToNext();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        seeking_ = false;
      }
      condition_.notify_all();
    } else if (seek && live_) {
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
    // Events from calls (play, pause, subtitles) go out at once.
    condition_.wait_for(
        lock, IdleLocked() ? kIdleControlPoll : kControlPoll, [this] {
          return quit_ || seek_pending_ || track_request_.pending() ||
                 live_change_.pending() || advance_pending_ || !events_.empty();
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
      // The control thread waits for that to seek or change tracks.
      if (seeking_) {
        condition_.notify_all();
      }
      // Queues above kReadWakeDuration drain without a notification.
      condition_.wait_for(lock, kReadPoll,
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

bool Player::IdleLocked() const {
  if (live_ || clock_running_ || prerolling_ || seek_in_progress_) {
    return false;
  }
  if (state_ != PlayerState::kReady && state_ != PlayerState::kEnded &&
      state_ != PlayerState::kError) {
    return false;
  }
  // A pause's fade-out still playing, or a replaced renderer's.
  return retired_audio_.empty() &&
         (!audio_renderer_ || audio_renderer_->SinkStopped());
}

bool Player::ShouldReadLocked() const {
  // Once the joined item is due, the control thread waits for a moment
  // outside ReadPacket() to make it the one playing.
  if (join_ && clock_.Now() >= join_->at) {
    return false;
  }
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

  // An adaptive stream starts on the variant automatic selection plays.
  std::vector<VariantInfo> variants = demuxer_->variants();
  int video_index = info.default_video_stream;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const int stream = AutoVariantStream(variants, max_bitrate_);
        stream >= 0) {
      video_index = stream;
    }
  }
  VideoRenderer::Ptr video;
  std::unique_ptr<AudioRenderer> audio;
  std::map<int, std::unique_ptr<SubtitleRenderer>> subtitles;
  if (video_index >= 0 && config_.video_frame_types != 0) {
    video = CreateVideoRenderer(info.streams[video_index]);
    // Set before this thread started, by SwitchSource().
    if (video && continue_shown_ != kNoTimestamp) {
      video->ContinueFrom(info.start_time + continue_shown_);
    }
  }
  if (info.default_audio_stream >= 0) {
    audio = CreateAudioRenderer(info.streams[info.default_audio_stream]);
  }
  if (!video && !audio) {
    *error = NoPlayableStream(info);
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
  // Live HLS starts near the end of its window.
  const int64_t start = info.start_time + info.start_position;
  clock_.Reset(start);

  std::lock_guard<std::mutex> lock(mutex_);
  // A live input before may have left it at 1.
  clock_.SetRate(rate_);
  media_info_ = info;
  variants_ = std::move(variants);
  histories_ = std::move(histories);
  video_stream_ = video ? video_index : -1;
  audio_stream_ = audio ? info.default_audio_stream : -1;
  // A subtitle file added while the input opened stays selected.
  if (subtitle_stream_ < kExternalSubtitleStream) {
    subtitle_stream_ = subtitles.count(info.default_subtitle_stream)
                           ? info.default_subtitle_stream
                           : -1;
  }
  video_renderer_ = std::move(video);
  audio_renderer_ = std::move(audio);
  subtitle_renderers_ = std::move(subtitles);
  EnableStreamsLocked();
  if (audio_renderer_) {
    audio_renderer_->SetVolume(volume_);
    audio_renderer_->SetGain(gain_, gain_peak_);
    audio_renderer_->SetRate(rate_);
    audio_renderer_->Start(live_ ? kNoTimestamp : start);
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
    video_rotation_ = config.rotation;
    PostEventLocked(PlayerEvent::kVideoSizeChanged, video_width_,
                    video_height_);
  } else {
    video_width_ = video_height_ = video_rotation_ = 0;
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
      if (!request.new_item) {
        video->ContinueFrom(shown_end);
      }
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
      video_rotation_ = config.rotation;
    } else {
      video_width_ = video_height_ = video_rotation_ = 0;
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
      audio_renderer_->SetGain(gain_, gain_peak_);
      audio_renderer_->SetRate(rate_);
      // The resync seek that follows sets where it starts.
      audio_renderer_->Start(kNoTimestamp);
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  EnableStreamsLocked();
  // A live input may have no tracks for a while.
  if (!video_renderer_ && !audio_renderer_ && !live_) {
    FailLocked(NoPlayableStream(media_info_));
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
  if (result == DemuxerReadResult::kEndOfStream && (WrapLoop() || JoinNext())) {
    return;
  }
  // The read thread is the only writer of these while it runs. A joined
  // item has stream ids, subtitles and a timeline of its own until it
  // plays.
  const Join* join = join_.get();
  const int video_stream = join ? join->video_stream : video_stream_;
  const int audio_stream = join ? join->audio_stream : audio_stream_;
  const std::map<int, std::unique_ptr<SubtitleRenderer>>& subtitles =
      join ? join->subtitles : subtitle_renderers_;
  const int64_t offset =
      join ? join->base : item_base_ + (loops_ ? loops_ * loop_length_ : 0);
  if (result != DemuxerReadResult::kOk) {
    if (video_stream >= 0) {
      video_queue_.Push(EncodedPacket::CreateEndOfStream(video_stream));
    }
    if (audio_stream >= 0) {
      audio_queue_.Push(EncodedPacket::CreateEndOfStream(audio_stream));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    end_of_input_ = true;
    if (result == DemuxerReadResult::kError) {
      input_error_ = demuxer_->error();
      LOGW("input failed (%s); playing what was read", input_error_.c_str());
    }
    const MediaInfo& info = join ? join->info : media_info_;
    if (info.duration != kNoTimestamp) {
      video_last_pts_ = audio_last_pts_ =
          info.start_time + info.duration + offset;
    }
    return;
  }

  if (packet->pts != kNoTimestamp && (packet->stream_index == video_stream ||
                                      packet->stream_index == audio_stream)) {
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
  if (offset && (packet->stream_index == video_stream ||
                 packet->stream_index == audio_stream)) {
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
  const int variant = demuxer_->current_variant();
  int64_t pts = packet->pts;
  if (packet->stream_index == video_stream) {
    video_bytes_read_.fetch_add(static_cast<int64_t>(packet->size()),
                                std::memory_order_relaxed);
    video_queue_.Push(std::move(packet));
    if (pts != kNoTimestamp) {
      std::lock_guard<std::mutex> lock(mutex_);
      video_last_pts_ = std::max(video_last_pts_, pts);
      if (variant >= 0 && (variant_switches_.empty() ||
                           variant_switches_.back().second != variant)) {
        variant_switches_.emplace_back(pts, variant);
      }
      // Keep the one playing.
      const int64_t now = clock_.Now();
      while (variant_switches_.size() > 1 &&
             variant_switches_[1].first <= now) {
        variant_switches_.pop_front();
      }
    }
  } else if (packet->stream_index == audio_stream) {
    audio_bytes_read_.fetch_add(static_cast<int64_t>(packet->size()),
                                std::memory_order_relaxed);
    audio_queue_.Push(std::move(packet));
    if (pts != kNoTimestamp) {
      std::lock_guard<std::mutex> lock(mutex_);
      audio_last_pts_ = std::max(audio_last_pts_, pts);
    }
  } else if (auto it = subtitles.find(packet->stream_index);
             it != subtitles.end()) {
    it->second->Decode(*packet);
    int64_t ended_by = kNoTimestamp;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!(join ? join->info : media_info_).seekable) {
        const int64_t now = clock_.Now();
        ended_by = now - LoopOffsetLocked(now) - subtitle_delay_ -
                   kUnseekableSubtitleHistory;
      }
    }
    if (ended_by != kNoTimestamp) {
      it->second->DropEndedBy(ended_by);
    }
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
    // Nor does an item joined to the one playing before it has started:
    // the control thread loops it by seeking.
    if (!looping_ || live_ || join_ || !media_info_.seekable ||
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
      input_error_ = ErrorTagOf(demuxer_->error()) + "loop seek failed";
    }
    return false;
  }
  if (loops_ == 0) {
    SetLoopSpan();
  }
  // Each pass lasts |loop_length_|: audio past it (encoder padding) is
  // cut, and a shorter track is padded with silence, so passes neither
  // overlap nor leave gaps.
  const int64_t boundary =
      item_base_ + loop_start_ + (loops_ + 1) * loop_length_;
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
  PublishLoopMapLocked();
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
  PublishLoopMapLocked();
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
  // by as much. Other seeks go back to the file's timeline.
  const int64_t media_target = file_target + (resync ? item_base_ : 0) +
                               (loops ? loops * loop_length_ : 0);
  LOGI("%s to %lld us", loop ? "loop" : "seek", static_cast<long long>(target));
  // The read starts the audio's preroll early (as Chromium's FFmpegDemuxer
  // does for Opus); the audio renderer cuts what comes before the target.
  int64_t demux_target = file_target;
  if (audio_stream_ >= 0 &&
      static_cast<size_t>(audio_stream_) < media_info_.streams.size()) {
    demux_target = std::max(
        media_info_.start_time,
        file_target -
            SeekPreroll(media_info_.streams[audio_stream_].audio.codec));
  }
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
    demuxer_->Seek(demux_target);
  } else if (!demuxer_->Seek(demux_target)) {
    std::lock_guard<std::mutex> lock(mutex_);
    seek_in_progress_ = false;
    current_seek_reported_ = false;
    // A newer request or shutdown can interrupt this seek. Otherwise the
    // demuxer's read position is no longer trustworthy, even when the
    // renderers still have buffered data from before the seek.
    if (!quit_ && !seek_pending_ && !track_request_.pending() &&
        state_ != PlayerState::kError) {
      FailLocked(ErrorTagOf(demuxer_->error()) + "seek failed");
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
  variant_switches_.clear();
  input_error_.clear();
  waiting_for_next_ = false;
  loops_ = loops;
  if (!resync) {
    item_base_ = 0;
  }
  loop_end_ = kNoTimestamp;
  PublishLoopMapLocked();
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
  // The control thread alone replaces the renderers (StopPipeline() only
  // once it has joined it), so it uses them here without |mutex_|, before
  // and after the state machine: these calls can block in the platform
  // (a sink starting or stopping, MediaCodec taking back the frames that
  // expire), and every caller waiting for |mutex_| would wait with them.
  if (video_renderer_) {
    video_renderer_->ExpireFramesWhileHidden(NowMicros());
  }
  if (audio_renderer_) {
    audio_renderer_->PullForStalledDevice(NowMicros());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    UpdateStateLocked();
  }
  // Starts or stops the sinks as the state machine, a seek or a track
  // change decided.
  const int64_t now = NowMicros();
  if (audio_renderer_) {
    audio_renderer_->UpdateSink(now);
  }
  for (auto it = retired_audio_.begin(); it != retired_audio_.end();) {
    (*it)->UpdateSink(now);
    if ((*it)->SinkStopped()) {
      (*it)->Stop();
      it = retired_audio_.erase(it);
    } else {
      ++it;
    }
  }
}

void Player::UpdateStateLocked() {
  CheckVideoRendererLocked();
  if (state_ != PlayerState::kError) {
    if (video_renderer_ && video_renderer_->HasError()) {
      FailLocked(TagError(ErrorKind::kDecode,
                          std::string("video decoding failed (") +
                              video_renderer_->decoder_name() + ")"));
    } else if (audio_renderer_ && audio_renderer_->HasError()) {
      FailLocked(TagError(ErrorKind::kDecode, "audio decoding failed"));
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
      if (waiting_for_next_) {
        if (!next_) {
          waiting_for_next_ = false;
          SetStateLocked(PlayerState::kEnded);
        } else if (next_->done) {
          waiting_for_next_ = false;
          advance_pending_ = true;
        }
      } else if (PrerollDoneLocked() &&
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
      } else if (seek_pending_ || advance_pending_) {
        // Wait for the seek; the ended and starved checks see stale data.
      } else if (EndedLocked()) {
        if (!input_error_.empty()) {
          FailLocked(input_error_);
        } else if (looping_ && media_info_.seekable) {
          seek_pending_ = true;
          seek_target_ = 0;
          loop_seek_pending_ = true;
          report_seek_ = false;
        } else if (next_ && next_->done) {
          // The next item did not join: switch to it now.
          advance_pending_ = true;
        } else if (next_) {
          LOGI("waiting for the next item to open");
          waiting_for_next_ = true;
          SetStateLocked(PlayerState::kBuffering);
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
  if (const int variant = PlayingVariantLocked();
      variant >= 0 && variant != variant_logged_) {
    const int64_t now = clock_.Now();
    LOGI("variant of %lld bit/s plays from %lld ms",
         static_cast<long long>(variants_[variant].bandwidth),
         Millis(now - LoopOffsetLocked(now) - media_info_.start_time));
    variant_logged_ = variant;
    PostEventLocked(PlayerEvent::kVariantChanged, variant);
  }
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
    const int64_t now = SubtitleTimeLocked(clock_.Now());
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
  // late, as a cue shown from a time past its start. Likewise a subtitle
  // file still loading, whose cues would come in late.
  if (!events_.empty() || dispatching_ || subtitle_loads_pending_ > 0) {
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
      track_request_.pending() || advance_pending_) {
    return false;
  }
  // The control thread has yet to make the joined item the one playing.
  if (join_ && clock_.Now() >= join_->at) {
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
    const int64_t now = SubtitleTimeLocked(clock_.Now());
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
  error_code_ = TakeErrorCode(&error_);
  SetStateLocked(PlayerState::kError);
  PostEventLocked(PlayerEvent::kError, static_cast<int64_t>(error_code_.kind),
                  error_code_.http_status);
}

}  // namespace lychee
