#include "media/null_audio_sink.h"

#include <algorithm>
#include <mutex>

#include "base/time.h"

namespace lychee {

namespace {

// How far ahead of the current time audio is handed out, like a device's
// buffer.
constexpr int64_t kLatency = 20 * kMicrosPerMilli;
// Frames per render callback.
constexpr int kPeriodFrames = 512;

// The started sinks. Held while rendering, so Stop() returns only after any
// render in progress. Lock order: this, then the renderer's locks; Start()
// and Stop() are called with the player's lock, which renders never take.
std::mutex g_mutex;
std::vector<NullAudioSink*>& Sinks() {
  static std::vector<NullAudioSink*> sinks;
  return sinks;
}

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

bool NullAudioSink::Open(RenderCallback callback, std::string*) {
  callback_ = std::move(callback);
  return true;
}

AudioOutputFormat NullAudioSink::format() const {
  return format_;
}

void NullAudioSink::Start() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto& sinks = Sinks();
  if (std::find(sinks.begin(), sinks.end(), this) != sinks.end()) {
    return;
  }
  start_time_ = NowMicros();
  frames_rendered_ = 0;
  sinks.push_back(this);
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
    callback_(buffer_.data(), kPeriodFrames, playout_time);
    frames_rendered_ += kPeriodFrames;
  }
}

}  // namespace lychee
