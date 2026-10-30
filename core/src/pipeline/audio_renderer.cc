#define LOG_TAG "audio_renderer"

#include "pipeline/audio_renderer.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <thread>

#include "base/log.h"
#include "base/time.h"
#include "base/thread_name.h"
#include "media/factory.h"

namespace lychee {

namespace {

// Decode ahead at most this much.
constexpr int64_t kMaxBufferedDuration = kMicrosPerSecond;
// Enough to start playing.
constexpr int64_t kPrerollDuration = 200 * kMicrosPerMilli;
// Live: a sample this late is still played, so a device clock running a
// little slow does not cut every buffer.
constexpr int64_t kLiveLateTolerance = 2 * kMicrosPerMilli;
// A device that pulls nothing for this long while playing is given up on.
// Bluetooth headphones take ~300 ms to start.
constexpr int64_t kDeviceTimeout = 2 * kMicrosPerSecond;
// Audio pulled for a stalled device reaches this far past now, as a
// device's buffer does, so the clock does not stop between pulls.
constexpr int64_t kStalledDeviceLead = 50 * kMicrosPerMilli;
// A seek or pause fades out the audio playing, and the audio after it in,
// over this long: short enough not to be heard as a fade, long enough not
// to click.
constexpr int64_t kFadeDuration = 10 * kMicrosPerMilli;
// Decoded audio whose timestamp is off by at most this much from where the
// audio before it ended continues it: the container rounded the timestamp
// (Matroska keeps milliseconds, and Opus's first block starts 0.5 ms off
// one). Cut by its own timestamp at a loop's end, each pass would lose
// that much and the next start that far off in phase.
constexpr int64_t kTimestampJitter = 2 * kMicrosPerMilli;
// A paused device that has not taken the fade-out by then is stopped
// without it.
constexpr int64_t kFadeOutTimeout = 500 * kMicrosPerMilli;
// How long a stop waits for the device to play its fade-out: the fade,
// and the device's latency up to a point.
constexpr auto kStopFadeTimeout = std::chrono::milliseconds(100);

// A sink that failed to start is started again this often while
// playing; the stalled device's audio is pulled for meanwhile.
constexpr int64_t kSinkStartRetry = 1 * kMicrosPerSecond;

std::atomic<bool> g_devices_stalled{false};
std::atomic<int> g_failed_sink_starts{0};

}  // namespace

AudioRenderer::AudioRenderer(MediaClock* clock,
                             std::atomic<int64_t>* live_output_lead)
    : clock_(clock), live_output_lead_(live_output_lead) {}

AudioRenderer::~AudioRenderer() {
  Stop();
  sink_.reset();
}

bool AudioRenderer::Initialize(const AudioConfig& config,
                               PacketQueue* queue,
                               std::string* error) {
  queue_ = queue;
  sink_ = CreateAudioSink();
  if (!sink_) {
    *error = "no audio sink in this build";
    return false;
  }
  if (!sink_->Open(
          [this](float* destination, int frames, int64_t playout_time) {
            if (g_devices_stalled.load(std::memory_order_relaxed)) {
              return 0;
            }
            return Render(destination, frames, playout_time, true);
          },
          error)) {
    return false;
  }
  format_ = sink_->format();
  algorithm_.Initialize(format_.channels, format_.sample_rate);
  render_bus_ = AudioBus(format_.channels, 0);
  for (auto& decoder : CreateAudioDecoders()) {
    std::string decoder_error;
    if (decoder->Initialize(config, format_, &decoder_error)) {
      decoder_ = std::move(decoder);
      return true;
    }
    LOGI("%s decoder rejected %s: %s", decoder->name(),
         config.codec_name.c_str(), decoder_error.c_str());
    *error = decoder_error;
  }
  if (error->empty()) {
    *error = "no audio decoder in this build";
  }
  return false;
}

void AudioRenderer::Start(int64_t start_time) {
  start_time_ = start_time;
  thread_ = std::thread(&AudioRenderer::DecodeLoop, this);
}

void AudioRenderer::StopDecoding() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void AudioRenderer::Retire() {
  SetPlaying(false);
  StopDecoding();
}

bool AudioRenderer::SinkStopped() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !sink_running_;
}

void AudioRenderer::FadeOutForStop() {
  SetPlaying(false);
  const auto give_up = std::chrono::steady_clock::now() + kStopFadeTimeout;
  while (!SinkStopped() && std::chrono::steady_clock::now() < give_up) {
    StopSinkAfterFadeOut(NowMicros());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void AudioRenderer::Stop() {
  StopDecoding();
  if (sink_) {
    sink_->Stop();
  }
}

void AudioRenderer::DecodeLoop() {
  SetCurrentThreadName("lychee-adecode");
  uint64_t decoder_generation = 0;
  // Where the decoded audio so far ends: |anchor_pts| plus
  // |anchor_frames| (Chromium's AudioDiscardHelper likewise counts frames
  // from the first timestamp on). Counted in frames, since a buffer's
  // duration in microseconds is rounded.
  int64_t anchor_pts = kNoTimestamp;
  int64_t anchor_frames = 0;
  PacketQueue::Entry entry;
  while (queue_->Pop(&entry)) {
    if (entry.generation != decoder_generation) {
      decoder_->Flush();
      decoder_generation = entry.generation;
      anchor_pts = kNoTimestamp;
    }
    uint64_t generation = entry.generation;
    auto output = [this, generation, &anchor_pts,
                   &anchor_frames](std::shared_ptr<AudioBuffer> buffer) {
      if (buffer->pts != kNoTimestamp) {
        const int64_t next_pts = anchor_pts == kNoTimestamp
                                     ? kNoTimestamp
                                     : anchor_pts + anchor_frames *
                                                        kMicrosPerSecond /
                                                        buffer->sample_rate;
        if (next_pts != kNoTimestamp &&
            std::abs(buffer->pts - next_pts) <= kTimestampJitter) {
          buffer->pts = next_pts;
        } else {
          anchor_pts = buffer->pts;
          anchor_frames = 0;
        }
        anchor_frames += buffer->frames();
      }
      OnBuffer(std::move(buffer), generation);
    };
    if (entry.packet->boundary) {
      // A loop: play this pass up to the boundary and the next from there,
      // as if seeking to it.
      decoder_->Decode(*EncodedPacket::CreateEndOfStream(-1), output);
      decoder_->Restart();
      anchor_pts = kNoTimestamp;
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation == generation_) {
        algorithm_.EndInputAt(entry.packet->pts);
        start_time_ = entry.packet->pts;
      }
      continue;
    }
    bool ok = decoder_->Decode(*entry.packet, output);
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

void AudioRenderer::OnBuffer(std::shared_ptr<AudioBuffer> buffer,
                             uint64_t generation) {
  std::unique_lock<std::mutex> lock(mutex_);
  waiting_ = true;
  condition_.wait(lock, [&] {
    return stopped_ || generation != generation_ ||
           BufferedDurationLocked() < kMaxBufferedDuration;
  });
  waiting_ = false;
  if (stopped_ || generation != generation_) {
    return;
  }
  if (live_output_lead_) {
    live_frames_ += buffer->frames();
    live_buffers_.push_back(std::move(buffer));
    return;
  }
  // Accurate seek: drop audio before the seek target.
  if (start_time_ != kNoTimestamp && buffer->pts != kNoTimestamp) {
    int64_t end = buffer->pts + buffer->duration();
    if (end <= start_time_) {
      return;
    }
    if (buffer->pts < start_time_) {
      // Rounded as EndInputAt() rounds the end of a pass, or a loop's
      // passes would differ by a frame.
      int skip = static_cast<int>(
          std::llround(static_cast<double>(start_time_ - buffer->pts) *
                       buffer->sample_rate / kMicrosPerSecond));
      skip = std::min(skip, buffer->frames());
      buffer->samples.erase(
          buffer->samples.begin(),
          buffer->samples.begin() +
              static_cast<ptrdiff_t>(skip) * buffer->channels);
      buffer->pts += static_cast<int64_t>(skip) * kMicrosPerSecond /
                     buffer->sample_rate;
    }
  }
  algorithm_.EnqueueBuffer(std::move(buffer));
}

int AudioRenderer::Render(float* destination,
                          int frames,
                          int64_t playout_time,
                          bool from_device) {
  const float volume = volume_.load();
  int written = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Under the lock: SetRate() fades out at the rate before it.
    const double rate = rate_.load();
    if (from_device) {
      device_pull_time_ = NowMicros();
      if (stalled_playout_time_ != kNoTimestamp) {
        LOGI("audio device pulls again");
        stalled_playout_time_ = kNoTimestamp;
      }
    } else if (stalled_playout_time_ == kNoTimestamp) {
      // The device took over meanwhile.
      return 0;
    }
    if (render_bus_.frames() < frames) {
      render_bus_.Resize(frames);
    }
    // First what is left of the fade-out of the audio a seek or pause cut.
    const int faded = std::min(frames, fade_out_frames_ - fade_out_offset_);
    if (faded > 0) {
      fade_out_.CopyPartialFramesTo(fade_out_offset_, faded, 0, &render_bus_);
      fade_out_offset_ += faded;
      playout_time +=
          static_cast<int64_t>(faded) * kMicrosPerSecond / format_.sample_rate;
      if (fade_out_offset_ == fade_out_frames_ && !playing_) {
        stop_sink_at_ = playout_time;
      }
    }
    // Then the audio playing, if any.
    const int requested = playing_ ? frames - faded : 0;
    int filled = 0;
    if (requested > 0 && live_output_lead_) {
      live_output_lead_->store(playout_time - NowMicros(),
                               std::memory_order_relaxed);
      filled = RenderLiveLocked(requested, playout_time);
    } else if (requested > 0) {
      filled = algorithm_.FillBuffer(
          &render_bus_, faded, std::min(requested, FramesBeforeEndLocked(rate)),
          rate);
      const int fade_frames = FadeFrames();
      for (int k = 0; k < format_.channels && fade_in_frames_ < fade_frames;
           ++k) {
        float* samples = render_bus_.channel(k) + faded;
        for (int i = 0; i < filled && fade_in_frames_ + i < fade_frames; ++i) {
          samples[i] *= static_cast<float>(fade_in_frames_ + i + 1) /
                        static_cast<float>(fade_frames);
        }
      }
      fade_in_frames_ = std::min(fade_frames, fade_in_frames_ + filled);
    }
    written = faded + filled;
    if (requested == 0) {
      // Paused, or the fade-out filled the buffer: nothing for the clock.
    } else if (filled == 0 && end_of_stream_) {
      // WSOLA keeps less than a window it cannot use: drop it so the stream
      // can end. Then let video (if any) run on the system clock.
      algorithm_.FlushBuffers();
      clock_->ClearMaxTime();
    } else if (filled == 0 && FramesBeforeEndLocked(rate) == 0) {
      clock_->ClearMaxTime();
    } else if (const int64_t end_pts = algorithm_.NextOutputTimestamp();
               filled > 0 && end_pts != kNoTimestamp && !live_output_lead_) {
      // Under the lock: a device taking over from PullForStalledDevice()
      // must not have its update overwritten by an older one.
      // |filled| frames at |rate| cover this much media time.
      const int64_t span = static_cast<int64_t>(
          filled * rate * kMicrosPerSecond / format_.sample_rate);
      clock_->UpdateFromAudio(end_pts - span, playout_time, end_pts);
      // The last of the audio: the clock runs on past its end now, not
      // from the next pull, which finds nothing. That pull is a device
      // period later (a vsync on the virtual clock), and the clock would
      // stand still at the end meanwhile, holding the video frame on
      // screen.
      if (end_of_stream_ && algorithm_.BufferedFrames() == 0) {
        clock_->ClearMaxTime();
      }
    }
    // PullForStalledDevice() drops what it pulls.
    if (destination) {
      const int channels = format_.channels;
      for (int k = 0; k < channels; ++k) {
        const float* source = render_bus_.channel(k);
        for (int i = 0; i < written; ++i) {
          destination[i * channels + k] = source[i] * volume;
        }
      }
    }
  }
  condition_.notify_all();
  return written;
}

int AudioRenderer::RenderLiveLocked(int frames, int64_t playout_time) {
  const int rate = format_.sample_rate;
  const int channels = format_.channels;
  int written = 0;
  while (written < frames && !live_buffers_.empty()) {
    const AudioBuffer& buffer = *live_buffers_.front();
    const int available = buffer.frames() - live_offset_;
    if (available <= 0 || buffer.pts == kNoTimestamp) {
      live_frames_ -= available;
      live_buffers_.pop_front();
      live_offset_ = 0;
      continue;
    }
    const int64_t due =
        playout_time + static_cast<int64_t>(written) * kMicrosPerSecond / rate;
    const int64_t pts =
        buffer.pts + static_cast<int64_t>(live_offset_) * kMicrosPerSecond / rate;
    if (pts < due - kLiveLateTolerance) {
      // Too late to be heard in time.
      const int late = static_cast<int>((due - pts) * rate / kMicrosPerSecond);
      const int skip = std::clamp(late, 1, available);
      live_offset_ += skip;
      live_frames_ -= skip;
      continue;
    }
    int count;
    if (pts - due > kMicrosPerSecond / rate) {
      // Not due yet: silence until it is.
      count = std::min(
          frames - written,
          static_cast<int>((pts - due) * rate / kMicrosPerSecond));
      for (int k = 0; k < channels; ++k) {
        std::fill_n(render_bus_.channel(k) + written, count, 0.0f);
      }
    } else {
      count = std::min(frames - written, available);
      const float* source =
          buffer.samples.data() + static_cast<size_t>(live_offset_) * channels;
      for (int k = 0; k < channels; ++k) {
        float* destination = render_bus_.channel(k) + written;
        for (int i = 0; i < count; ++i) {
          destination[i] = source[i * channels + k];
        }
      }
      live_offset_ += count;
      live_frames_ -= count;
    }
    written += count;
  }
  return written;
}

void AudioRenderer::Flush(uint64_t generation, int64_t start_time) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_) {
      FadeOutLocked();
    }
    fade_in_frames_ = 0;
    generation_ = generation;
    start_time_ = start_time;
    algorithm_.FlushBuffers();
    live_buffers_.clear();
    live_offset_ = 0;
    live_frames_ = 0;
    end_time_ = kNoTimestamp;
    end_of_stream_ = false;
  }
  condition_.notify_all();
}

void AudioRenderer::SetPlaying(bool playing) {
  bool start = false;
  bool stop = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_ == playing) {
      return;
    }
    playing_ = playing;
    const int64_t now = NowMicros();
    if (playing) {
      // The device gets kDeviceTimeout from here, or a stalled one is
      // pulled for from here on.
      device_pull_time_ = now;
      if (stalled_playout_time_ != kNoTimestamp) {
        stalled_playout_time_ = now;
      }
      fade_in_frames_ = 0;
      stop_sink_at_ = kNoTimestamp;
      start = !sink_running_;
      sink_running_ = true;
    } else {
      FadeOutLocked();
      if (fade_out_offset_ < fade_out_frames_ && !sink_start_failed_) {
        // StopSinkAfterFadeOut() stops it.
        stop_sink_at_ = now + kFadeOutTimeout;
      } else {
        stop = sink_running_;
        sink_running_ = false;
        sink_start_failed_ = false;
      }
    }
  }
  if (start && !StartSink()) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_start_failed_ = sink_running_;
    sink_start_retry_at_ = NowMicros() + kSinkStartRetry;
  }
  if (stop) {
    sink_->Stop();
  }
}

void AudioRenderer::StopSinkAfterFadeOut(int64_t now) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_ || !sink_running_ || stop_sink_at_ == kNoTimestamp ||
        now < stop_sink_at_) {
      return;
    }
    sink_running_ = false;
    sink_start_failed_ = false;
    stop_sink_at_ = kNoTimestamp;
    fade_out_offset_ = fade_out_frames_;
  }
  sink_->Stop();
}

int AudioRenderer::FadeFrames() const {
  return static_cast<int>(kFadeDuration * format_.sample_rate /
                          kMicrosPerSecond);
}

void AudioRenderer::FadeOutLocked() {
  // Live input plays each sample when it is due; one already fading out
  // goes on.
  if (live_output_lead_ || fade_out_offset_ < fade_out_frames_) {
    return;
  }
  const int frames = FadeFrames();
  if (fade_out_.frames() < frames) {
    fade_out_ = AudioBus(format_.channels, frames);
  }
  const double rate = rate_.load();
  fade_out_frames_ = algorithm_.FillBuffer(
      &fade_out_, 0, std::min(frames, FramesBeforeEndLocked(rate)), rate);
  fade_out_offset_ = 0;
  // From where a fade-in still under way has got to, down to silence.
  for (int k = 0; k < format_.channels; ++k) {
    float* samples = fade_out_.channel(k);
    for (int i = 0; i < fade_out_frames_; ++i) {
      const float in =
          std::min(1.0f, static_cast<float>(fade_in_frames_ + i + 1) /
                             static_cast<float>(frames));
      const float out = 1.0f - static_cast<float>(i + 1) /
                                   static_cast<float>(fade_out_frames_);
      samples[i] *= in * out;
    }
  }
}

bool AudioRenderer::StartSink() {
  int failures = g_failed_sink_starts.load();
  while (failures > 0) {
    if (g_failed_sink_starts.compare_exchange_weak(failures, failures - 1)) {
      LOGW("audio device failed to start (testing)");
      return false;
    }
  }
  return sink_->Start();
}

void AudioRenderer::RetrySinkStart(int64_t now) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sink_start_failed_ || now < sink_start_retry_at_) {
      return;
    }
  }
  // The control thread alone starts and stops the sink, so nothing
  // changed meanwhile.
  const bool started = StartSink();
  std::lock_guard<std::mutex> lock(mutex_);
  if (started) {
    LOGI("audio device started on retry");
    sink_start_failed_ = false;
  } else {
    sink_start_retry_at_ = now + kSinkStartRetry;
  }
}

void AudioRenderer::PullForStalledDevice(int64_t now) {
  RetrySinkStart(now);
  int64_t playout_time;
  int frames;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) {
      return;
    }
    if (stalled_playout_time_ == kNoTimestamp) {
      if (now - device_pull_time_ < kDeviceTimeout) {
        return;
      }
      LOGW("audio device pulled nothing for %lld ms; playing on without it",
           static_cast<long long>((now - device_pull_time_) / kMicrosPerMilli));
      stalled_playout_time_ = now;
    }
    playout_time = stalled_playout_time_;
    frames = static_cast<int>((now + kStalledDeviceLead - playout_time) *
                              format_.sample_rate / kMicrosPerSecond);
    if (frames <= 0) {
      return;
    }
    stalled_playout_time_ +=
        static_cast<int64_t>(frames) * kMicrosPerSecond / format_.sample_rate;
  }
  Render(nullptr, frames, playout_time, false);
}

void AudioRenderer::SetVolume(float volume) {
  volume_ = std::clamp(volume, 0.0f, 1.0f);
}

void AudioRenderer::SetRate(double rate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rate == rate_.load()) {
    return;
  }
  // The algorithm starts its output over when it switches between copying
  // the input and WSOLA: fade the old rate's out and the new one's in.
  if (playing_) {
    FadeOutLocked();
    fade_in_frames_ = 0;
  }
  rate_ = rate;
}

bool AudioRenderer::HasEnoughData() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return end_of_stream_ || BufferedDurationLocked() >= kPrerollDuration;
}

bool AudioRenderer::IsStarved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return algorithm_.BufferedFrames() == 0 && !end_of_stream_;
}

bool AudioRenderer::Ended() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return (end_of_stream_ && algorithm_.BufferedFrames() == 0) ||
         FramesBeforeEndLocked(rate_.load()) == 0;
}

void AudioRenderer::SetEndTime(int64_t time) {
  std::lock_guard<std::mutex> lock(mutex_);
  end_time_ = time;
}

int AudioRenderer::FramesBeforeEndLocked(double rate) const {
  const int64_t next = algorithm_.NextOutputTimestamp();
  if (end_time_ == kNoTimestamp || next == kNoTimestamp) {
    return INT_MAX;
  }
  const double frames = static_cast<double>(end_time_ - next) *
                        format_.sample_rate / kMicrosPerSecond / rate;
  return static_cast<int>(std::clamp(frames, 0.0, double{INT_MAX}));
}

bool AudioRenderer::HasDataFor(int64_t duration) const {
  // Past what it outputs, WSOLA needs a search interval and a window
  // (audio_renderer_algorithm.cc).
  constexpr int64_t kWsolaLookahead = 100 * kMicrosPerMilli;
  std::lock_guard<std::mutex> lock(mutex_);
  return end_of_stream_ || failed_ ||
         BufferedDurationLocked() >=
             std::min(duration + kWsolaLookahead, kMaxBufferedDuration / 2);
}

bool AudioRenderer::Blocked() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return waiting_ || failed_;
}

bool AudioRenderer::HasError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

int64_t AudioRenderer::buffered_duration() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return BufferedDurationLocked();
}

// static
void AudioRenderer::StallDevicesForTesting(bool stalled) {
  g_devices_stalled = stalled;
}

// static
void AudioRenderer::FailSinkStartsForTesting(int count) {
  g_failed_sink_starts = count;
}

int64_t AudioRenderer::BufferedDurationLocked() const {
  if (live_output_lead_) {
    return live_frames_ * kMicrosPerSecond / format_.sample_rate;
  }
  return static_cast<int64_t>(algorithm_.BufferedFrames()) *
         kMicrosPerSecond / format_.sample_rate;
}

}  // namespace lychee
