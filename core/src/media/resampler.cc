#include "media/resampler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lychee {

namespace {

// Taps per output sample, half before and half after it.
constexpr int kTaps = 32;
constexpr int kHalf = kTaps / 2;
// Fractional offsets with a precomputed kernel; offsets in between
// interpolate two neighbouring kernels.
constexpr int kPhases = 256;
// Of the lower Nyquist frequency: room for the window's transition band.
constexpr double kCutoff = 0.9;

}  // namespace

Resampler::Resampler(int channels, int input_rate, int output_rate)
    : channels_(channels),
      input_rate_(input_rate),
      output_rate_(output_rate) {
  // Downsampling filters below the output's Nyquist frequency.
  const double cutoff =
      kCutoff * std::min(1.0, static_cast<double>(output_rate) / input_rate);
  kernels_.resize(static_cast<size_t>(kPhases + 1) * kTaps);
  for (int phase = 0; phase <= kPhases; ++phase) {
    const double fraction = static_cast<double>(phase) / kPhases;
    float* kernel = &kernels_[static_cast<size_t>(phase) * kTaps];
    double sum = 0;
    for (int tap = 0; tap < kTaps; ++tap) {
      // Distance in input frames from the output time to this tap.
      const double x = tap - (kHalf - 1) - fraction;
      const double arg = std::numbers::pi * cutoff * x;
      const double sinc = x == 0 ? 1.0 : std::sin(arg) / arg;
      // Blackman, zero at +-kHalf.
      const double angle = std::numbers::pi * x / kHalf;
      const double window =
          0.42 + 0.5 * std::cos(angle) + 0.08 * std::cos(2 * angle);
      kernel[tap] = static_cast<float>(sinc * window);
      sum += kernel[tap];
    }
    // Unity gain at DC for every offset.
    for (int tap = 0; tap < kTaps; ++tap) {
      kernel[tap] = static_cast<float>(kernel[tap] / sum);
    }
  }
  Reset();
}

void Resampler::Reset() {
  // Silence before the first frame, so it gets a full set of taps.
  buffer_.assign(static_cast<size_t>(kHalf - 1) * channels_, 0.0f);
  time_ = (kHalf - 1) * output_rate_;
}

void Resampler::Push(const float* input, int frames,
                     std::vector<float>* output) {
  buffer_.insert(buffer_.end(), input,
                 input + static_cast<size_t>(frames) * channels_);
  Produce(static_cast<int64_t>(buffer_.size() / channels_), output);
  // Keep only what the next output frame's taps reach back to.
  const int64_t consumed = time_ / output_rate_ - (kHalf - 1);
  if (consumed > 0) {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() + static_cast<ptrdiff_t>(consumed) * channels_);
    time_ -= consumed * output_rate_;
  }
}

void Resampler::Drain(std::vector<float>* output) {
  const auto end = static_cast<int64_t>(buffer_.size() / channels_);
  buffer_.resize(buffer_.size() + static_cast<size_t>(kHalf) * channels_, 0.0f);
  Produce(end, output);
  Reset();
}

void Resampler::Produce(int64_t end, std::vector<float>* output) {
  const auto frames = static_cast<int64_t>(buffer_.size() / channels_);
  while (time_ < end * output_rate_ && time_ / output_rate_ + kHalf < frames) {
    const int64_t index = time_ / output_rate_;
    const double position =
        static_cast<double>(time_ % output_rate_) * kPhases / output_rate_;
    const int phase = static_cast<int>(position);
    const float mix = static_cast<float>(position - phase);
    const float* k0 = &kernels_[static_cast<size_t>(phase) * kTaps];
    const float* k1 = k0 + kTaps;
    const float* in =
        &buffer_[static_cast<size_t>(index - (kHalf - 1)) * channels_];
    for (int channel = 0; channel < channels_; ++channel) {
      float a = 0;
      float b = 0;
      for (int tap = 0; tap < kTaps; ++tap) {
        const float sample = in[tap * channels_ + channel];
        a += k0[tap] * sample;
        b += k1[tap] * sample;
      }
      output->push_back(a + (b - a) * mix);
    }
    time_ += input_rate_;
  }
}

std::vector<float> ChannelMatrix(int in, int out) {
  std::vector<float> matrix(static_cast<size_t>(in) * out, 0.0f);
  if (in == 1) {
    for (int o = 0; o < std::min(out, 2); ++o) {
      matrix[static_cast<size_t>(o) * in] = 1.0f;
    }
    return matrix;
  }
  if (in <= out) {
    for (int i = 0; i < in; ++i) {
      matrix[static_cast<size_t>(i) * in + i] = 1.0f;
    }
    return matrix;
  }
  if (out == 1) {
    // The non-LFE channels, averaged.
    const int count = in > 3 ? in - 1 : in;
    for (int i = 0; i < in; ++i) {
      if (i != 3) {
        matrix[i] = 1.0f / count;
      }
    }
    return matrix;
  }
  // To stereo: each side keeps its own channels, the center goes to both
  // at -3 dB, and so does any channel past the eighth.
  constexpr float kHalfPower = 0.70710678f;
  float left[8] = {1, 0, kHalfPower, 0, kHalfPower, 0, kHalfPower, 0};
  float right[8] = {0, 1, kHalfPower, 0, 0, kHalfPower, 0, kHalfPower};
  float left_sum = 0;
  float right_sum = 0;
  for (int i = 0; i < in; ++i) {
    const float l = i < 8 ? left[i] : kHalfPower;
    const float r = i < 8 ? right[i] : kHalfPower;
    matrix[i] = l;
    matrix[static_cast<size_t>(in) + i] = r;
    left_sum += l;
    right_sum += r;
  }
  const float scale = 1.0f / std::max(left_sum, right_sum);
  for (int i = 0; i < 2 * in; ++i) {
    matrix[i] *= scale;
  }
  return matrix;
}

}  // namespace lychee
