#define LOG_TAG "audio_renderer"

#include "pipeline/audio_renderer.h"

#include <algorithm>

#include "base/log.h"
#include "base/thread_name.h"
#include "media/factory.h"

namespace lychee {

namespace {

// Decode ahead at most this much.
constexpr int64_t kMaxBufferedDuration = kMicrosPerSecond;
// Enough to start playing.
constexpr int64_t kPrerollDuration = 200 * kMicrosPerMilli;

}  // namespace

AudioRenderer::AudioRenderer(MediaClock* clock) : clock_(clock) {}

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
            return Render(destination, frames, playout_time);
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

void AudioRenderer::Start() {
  thread_ = std::thread(&AudioRenderer::DecodeLoop, this);
}

void AudioRenderer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }
  condition_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  if (sink_) {
    sink_->Stop();
  }
}

void AudioRenderer::DecodeLoop() {
  SetCurrentThreadName("lychee-adecode");
  uint64_t decoder_generation = 0;
  PacketQueue::Entry entry;
  while (queue_->Pop(&entry)) {
    if (entry.generation != decoder_generation) {
      decoder_->Flush();
      decoder_generation = entry.generation;
    }
    uint64_t generation = entry.generation;
    bool ok = decoder_->Decode(
        *entry.packet, [this, generation](std::shared_ptr<AudioBuffer> buffer) {
          OnBuffer(std::move(buffer), generation);
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

void AudioRenderer::OnBuffer(std::shared_ptr<AudioBuffer> buffer,
                             uint64_t generation) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [&] {
    return stopped_ || generation != generation_ ||
           BufferedDurationLocked() < kMaxBufferedDuration;
  });
  if (stopped_ || generation != generation_) {
    return;
  }
  // Accurate seek: drop audio before the seek target.
  if (start_time_ != kNoTimestamp && buffer->pts != kNoTimestamp) {
    int64_t end = buffer->pts + buffer->duration();
    if (end <= start_time_) {
      return;
    }
    if (buffer->pts < start_time_) {
      int skip = static_cast<int>((start_time_ - buffer->pts) *
                                  buffer->sample_rate / kMicrosPerSecond);
      skip = std::min(skip, buffer->frames());
      buffer->samples.erase(buffer->samples.begin(),
                            buffer->samples.begin() + skip * buffer->channels);
      buffer->pts += static_cast<int64_t>(skip) * kMicrosPerSecond /
                     buffer->sample_rate;
    }
  }
  algorithm_.EnqueueBuffer(std::move(buffer));
}

int AudioRenderer::Render(float* destination,
                          int frames,
                          int64_t playout_time) {
  const float volume = volume_.load();
  const double rate = rate_.load();
  int written = 0;
  int64_t end_pts = kNoTimestamp;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) {
      return 0;
    }
    if (render_bus_.frames() < frames) {
      render_bus_.Resize(frames);
    }
    written = algorithm_.FillBuffer(&render_bus_, 0, frames, rate);
    end_pts = algorithm_.NextOutputTimestamp();
    if (written == 0 && end_of_stream_) {
      // WSOLA keeps less than a window it cannot use: drop it so the stream
      // can end. Then let video (if any) run on the system clock.
      algorithm_.FlushBuffers();
      clock_->ClearMaxTime();
    }
  }
  condition_.notify_all();
  const int channels = format_.channels;
  for (int k = 0; k < channels; ++k) {
    const float* source = render_bus_.channel(k);
    for (int i = 0; i < written; ++i) {
      destination[i * channels + k] = source[i] * volume;
    }
  }
  if (written > 0 && end_pts != kNoTimestamp) {
    // |written| frames at |rate| cover this much media time.
    const int64_t span = static_cast<int64_t>(
        written * rate * kMicrosPerSecond / format_.sample_rate);
    clock_->UpdateFromAudio(end_pts - span, playout_time, end_pts);
  }
  return written;
}

void AudioRenderer::Flush(uint64_t generation, int64_t start_time) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    start_time_ = start_time;
    algorithm_.FlushBuffers();
    end_of_stream_ = false;
  }
  condition_.notify_all();
}

void AudioRenderer::SetPlaying(bool playing) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_ == playing) {
      return;
    }
    playing_ = playing;
  }
  if (playing) {
    sink_->Start();
  } else {
    sink_->Stop();
  }
}

void AudioRenderer::SetVolume(float volume) {
  volume_ = std::clamp(volume, 0.0f, 1.0f);
}

void AudioRenderer::SetRate(double rate) {
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
  return end_of_stream_ && algorithm_.BufferedFrames() == 0;
}

bool AudioRenderer::HasError() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

int64_t AudioRenderer::buffered_duration() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return BufferedDurationLocked();
}

int64_t AudioRenderer::BufferedDurationLocked() const {
  return static_cast<int64_t>(algorithm_.BufferedFrames()) *
         kMicrosPerSecond / format_.sample_rate;
}

}  // namespace lychee
