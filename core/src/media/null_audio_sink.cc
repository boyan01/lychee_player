#include "media/null_audio_sink.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "base/time.h"

namespace lychee {

namespace {

// How far ahead of the current time audio is handed out, like a device's
// buffer.
constexpr int64_t kLatency = 20 * kMicrosPerMilli;
// Frames per render callback.
constexpr int kPeriodFrames = 512;
// A sample above this is sound (FFmpeg's sine source peaks at 1/8).
constexpr float kQuiet = 0.05f;

// The started sinks. Held while rendering, so Stop() returns only after any
// render in progress. Lock order: this, then the renderer's locks; Start()
// and Stop() are called with the player's lock, which renders never take.
std::mutex g_mutex;
std::vector<NullAudioSink*>& Sinks() {
  static std::vector<NullAudioSink*> sinks;
  return sinks;
}
// Guarded by g_mutex.
int64_t g_first_sound = kNoTimestamp;
// StartCapture(): when the recording starts, kNoTimestamp if not
// capturing, and the interleaved samples (every sink has the same format).
int64_t g_capture_start = kNoTimestamp;
std::vector<float>& Capture() {
  static std::vector<float> capture;
  return capture;
}
// Ten minutes of 48 kHz stereo, 230 MB.
constexpr int64_t kMaxCaptureFrames = int64_t{10} * 60 * 48000;

}  // namespace

NullAudioSink::NullAudioSink() {
  format_.sample_rate = 48000;
  format_.channels = 2;
}

NullAudioSink::~NullAudioSink() {
  Stop();
}

void NullAudioSink::RenderAll(int64_t now) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (NullAudioSink* sink : Sinks()) {
    sink->RenderUntil(now);
  }
}

int64_t NullAudioSink::FirstSound() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_first_sound;
}

void NullAudioSink::StartCapture() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_capture_start = NowMicros();
  Capture().clear();
}

bool NullAudioSink::WriteCapture(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<float>& samples = Capture();
  // Up to now: silence after the last sink stopped.
  if (g_capture_start != kNoTimestamp) {
    const int64_t frames =
        std::min((NowMicros() - g_capture_start) * 48000 / kMicrosPerSecond,
                 kMaxCaptureFrames);
    if (frames * 2 > static_cast<int64_t>(samples.size())) {
      samples.resize(static_cast<size_t>(frames) * 2);
    }
  }
  FILE* file = fopen(path.c_str(), "wb");
  if (!file) {
    return false;
  }
  // RIFF WAVE with an IEEE float fmt chunk; every platform here is little
  // endian.
  const uint32_t sample_size = sizeof(float);
  const uint32_t data_size =
      static_cast<uint32_t>(samples.size()) * sample_size;
  const uint16_t format = 3;
  const uint16_t channels = 2;
  const uint32_t rate = 48000;
  const uint32_t byte_rate = rate * channels * sample_size;
  const uint16_t block_align = channels * sample_size;
  const uint16_t bits = 32;
  const uint32_t fmt_size = 16;
  const uint32_t riff_size = 4 + 8 + fmt_size + 8 + data_size;
  bool ok =
      fwrite("RIFF", 4, 1, file) == 1 && fwrite(&riff_size, 4, 1, file) == 1 &&
      fwrite("WAVEfmt ", 8, 1, file) == 1 &&
      fwrite(&fmt_size, 4, 1, file) == 1 && fwrite(&format, 2, 1, file) == 1 &&
      fwrite(&channels, 2, 1, file) == 1 && fwrite(&rate, 4, 1, file) == 1 &&
      fwrite(&byte_rate, 4, 1, file) == 1 &&
      fwrite(&block_align, 2, 1, file) == 1 && fwrite(&bits, 2, 1, file) == 1 &&
      fwrite("data", 4, 1, file) == 1 && fwrite(&data_size, 4, 1, file) == 1 &&
      fwrite(samples.data(), sizeof(float), samples.size(), file) ==
          samples.size();
  return fclose(file) == 0 && ok;
}

bool NullAudioSink::Open(RenderCallback callback, std::string*) {
  callback_ = std::move(callback);
  return true;
}

AudioOutputFormat NullAudioSink::format() const {
  return format_;
}

bool NullAudioSink::Start() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& sinks = Sinks();
  if (std::find(sinks.begin(), sinks.end(), this) != sinks.end()) {
    return true;
  }
  start_time_ = NowMicros();
  frames_rendered_ = 0;
  if (g_capture_start != kNoTimestamp) {
    capture_offset_ = (start_time_ - g_capture_start) * format_.sample_rate /
                      kMicrosPerSecond;
  }
  sinks.push_back(this);
  return true;
}

void NullAudioSink::Stop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& sinks = Sinks();
  sinks.erase(std::remove(sinks.begin(), sinks.end(), this), sinks.end());
}

void NullAudioSink::RenderUntil(int64_t now) {
  const int64_t target_frames =
      (now + kLatency - start_time_) * format_.sample_rate / kMicrosPerSecond;
  buffer_.resize(static_cast<size_t>(kPeriodFrames) * format_.channels);
  // In device-sized periods, as a real device asks for audio.
  while (frames_rendered_ + kPeriodFrames <= target_frames) {
    const int64_t playout_time =
        start_time_ +
        frames_rendered_ * kMicrosPerSecond / format_.sample_rate;
    const int written =
        std::clamp(callback_(buffer_.data(), kPeriodFrames, playout_time), 0,
                   kPeriodFrames);
    // As a device does, silence after what the callback wrote.
    std::fill(
        buffer_.begin() + static_cast<ptrdiff_t>(written) * format_.channels,
        buffer_.end(), 0.0f);
    if (g_capture_start != kNoTimestamp) {
      const int64_t at = capture_offset_ + frames_rendered_;
      if (at >= 0 && at + kPeriodFrames <= kMaxCaptureFrames) {
        std::vector<float>& capture = Capture();
        const size_t begin = static_cast<size_t>(at) * format_.channels;
        if (capture.size() < begin + buffer_.size()) {
          capture.resize(begin + buffer_.size(), 0.0f);
        }
        for (size_t i = 0; i < buffer_.size(); ++i) {
          capture[begin + i] += buffer_[i];
        }
      }
    }
    if (g_first_sound == kNoTimestamp) {
      for (size_t i = 0; i < buffer_.size(); ++i) {
        if (std::abs(buffer_[i]) > kQuiet) {
          const int64_t frame = static_cast<int64_t>(i) / format_.channels;
          g_first_sound =
              playout_time + frame * kMicrosPerSecond / format_.sample_rate;
          break;
        }
      }
    }
    frames_rendered_ += kPeriodFrames;
  }
}

}  // namespace lychee
