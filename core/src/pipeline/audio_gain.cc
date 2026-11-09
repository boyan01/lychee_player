#include "pipeline/audio_gain.h"

#include <algorithm>

namespace lychee {

void LevelRamp::Set(float target, int64_t frames) {
  target_ = target;
  if (frames <= 0 || target == current_) {
    current_ = target;
    remaining_ = 0;
    return;
  }
  // From where a ramp under way has got to.
  step_ = (target - current_) / static_cast<float>(frames);
  remaining_ = frames;
}

void LevelRamp::Skip(int64_t frames) {
  if (frames >= remaining_) {
    current_ = target_;
    remaining_ = 0;
  } else if (frames > 0) {
    remaining_ -= frames;
    current_ = target_ - step_ * static_cast<float>(remaining_);
  }
}

void AudioGain::SetVolume(float volume, int64_t frames) {
  // NaN is silence.
  volume_.Set(volume > 0 ? std::min(volume, 1.0f) : 0.0f, frames);
}

void AudioGain::SetGain(float gain, float peak, int64_t frames) {
  wanted_gain_ = std::max(gain, 0.0f);
  given_peak_ = std::max(peak, 0.0f);
  Update(frames);
}

void AudioGain::NotePeak(float peak, int64_t frames) {
  if (peak > decoded_peak_) {
    decoded_peak_ = peak;
    Update(frames);
  }
}

void AudioGain::Update(int64_t frames) {
  float gain = wanted_gain_;
  const float peak = std::max(given_peak_, decoded_peak_);
  if (gain > 1.0f && peak > 0) {
    gain = std::max(1.0f, std::min(gain, 1.0f / peak));
  }
  if (gain != gain_.target()) {
    gain_.Set(gain, frames);
  }
}

}  // namespace lychee
