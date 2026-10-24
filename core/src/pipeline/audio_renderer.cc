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
    if (!ok) {
      LOGE("%s decoder failed", decoder_->name());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if ((entry.packet->end_of_stream || !ok) && generation == generation_) {
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
           buffered_duration_ < kMaxBufferedDuration;
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
  buffered_duration_ += buffer->duration();
  buffers_.push_back(std::move(buffer));
}

int AudioRenderer::Render(float* destination,
                          int frames,
                          int64_t playout_time) {
  int written = 0;
  int64_t first_pts = kNoTimestamp;
  float volume = volume_.load();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) {
      return 0;
    }
    while (written < frames && !buffers_.empty()) {
      AudioBuffer& buffer = *buffers_.front();
      if (first_pts == kNoTimestamp && buffer.pts != kNoTimestamp) {
        first_pts = buffer.pts + static_cast<int64_t>(front_offset_) *
                                     kMicrosPerSecond / buffer.sample_rate -
                    static_cast<int64_t>(written) * kMicrosPerSecond /
                        format_.sample_rate;
      }
      int count = std::min(buffer.frames() - front_offset_, frames - written);
      const float* source =
          buffer.samples.data() + front_offset_ * buffer.channels;
      int samples = count * format_.channels;
      float* out = destination + written * format_.channels;
      for (int i = 0; i < samples; ++i) {
        out[i] = source[i] * volume;
      }
      written += count;
      front_offset_ += count;
      if (front_offset_ >= buffer.frames()) {
        buffered_duration_ -= buffer.duration();
        buffers_.pop_front();
        front_offset_ = 0;
      }
    }
    if (written == 0 && end_of_stream_) {
      // Audio is done; let video (if any) run on the system clock.
      clock_->ClearMaxTime();
    }
  }
  condition_.notify_all();
  if (written > 0 && first_pts != kNoTimestamp) {
    clock_->UpdateFromAudio(
        first_pts, playout_time,
        first_pts + static_cast<int64_t>(written) * kMicrosPerSecond /
                        format_.sample_rate);
  }
  return written;
}

void AudioRenderer::Flush(uint64_t generation, int64_t start_time) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    start_time_ = start_time;
    buffers_.clear();
    front_offset_ = 0;
    buffered_duration_ = 0;
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

bool AudioRenderer::HasEnoughData() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return end_of_stream_ || buffered_duration_ >= kPrerollDuration;
}

bool AudioRenderer::IsStarved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buffers_.empty() && !end_of_stream_;
}

bool AudioRenderer::Ended() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return end_of_stream_ && buffers_.empty();
}

}  // namespace lychee
