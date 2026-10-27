#include "media/resampler.h"

#include <cmath>
#include <numbers>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

std::vector<float> Sine(double frequency, int rate, int frames, int channels) {
  std::vector<float> samples;
  for (int i = 0; i < frames; ++i) {
    const double value = std::sin(2 * std::numbers::pi * frequency * i / rate);
    for (int c = 0; c < channels; ++c) {
      samples.push_back(static_cast<float>(c == 0 ? value : -value));
    }
  }
  return samples;
}

// The largest error against the sine at the output's times, away from the
// edges, where the filter sees silence.
double MaxError(const std::vector<float>& output,
                double frequency,
                int rate,
                int channels) {
  const int frames = static_cast<int>(output.size()) / channels;
  double error = 0;
  for (int i = 64; i < frames - 64; ++i) {
    const double value = std::sin(2 * std::numbers::pi * frequency * i / rate);
    error = std::max(error, std::fabs(output[i * channels] - value));
    error = std::max(error, std::fabs(output[i * channels + 1] + value));
  }
  return error;
}

TEST(Resampler, Upsamples44100To48000) {
  Resampler resampler(2, 44100, 48000);
  std::vector<float> input = Sine(1000, 44100, 44100, 2);
  std::vector<float> output;
  resampler.Push(input.data(), 44100, &output);
  resampler.Drain(&output);
  // One second in, one second out, without delay.
  EXPECT_EQ(output.size(), 48000u * 2);
  EXPECT_TRUE(MaxError(output, 1000, 48000, 2) < 1e-3);
}

TEST(Resampler, Downsamples48000To44100) {
  Resampler resampler(2, 48000, 44100);
  std::vector<float> input = Sine(5000, 48000, 48000, 2);
  std::vector<float> output;
  resampler.Push(input.data(), 48000, &output);
  resampler.Drain(&output);
  EXPECT_EQ(output.size(), 44100u * 2);
  EXPECT_TRUE(MaxError(output, 5000, 44100, 2) < 2e-3);
}

TEST(Resampler, RemovesWhatTheOutputRateCannotCarry) {
  // 30 kHz at 96 kHz would alias to 14.1 kHz at 44.1 kHz.
  Resampler resampler(2, 96000, 44100);
  std::vector<float> input = Sine(30000, 96000, 9600, 2);
  std::vector<float> output;
  resampler.Push(input.data(), 9600, &output);
  double peak = 0;
  for (size_t i = 128; i < output.size() - 128; ++i) {
    peak = std::max(peak, static_cast<double>(std::fabs(output[i])));
  }
  EXPECT_TRUE(peak < 0.01);
}

TEST(Resampler, ChunksMatchOnePush) {
  std::vector<float> input = Sine(440, 22050, 5000, 2);
  Resampler whole(2, 22050, 48000);
  std::vector<float> expected;
  whole.Push(input.data(), 5000, &expected);
  whole.Drain(&expected);

  Resampler chunked(2, 22050, 48000);
  std::vector<float> output;
  for (int offset = 0; offset < 5000;) {
    const int frames = std::min(5000 - offset, 1 + offset % 397);
    chunked.Push(input.data() + offset * 2, frames, &output);
    offset += frames;
  }
  chunked.Drain(&output);
  EXPECT_EQ(output.size(), expected.size());
  bool same = output.size() == expected.size();
  for (size_t i = 0; same && i < output.size(); ++i) {
    same = std::fabs(output[i] - expected[i]) < 1e-6;
  }
  EXPECT_TRUE(same);
}

TEST(Resampler, StartsOverAfterDrain) {
  Resampler resampler(1, 44100, 48000);
  std::vector<float> input(441, 1.0f);
  std::vector<float> first;
  resampler.Push(input.data(), 441, &first);
  resampler.Drain(&first);
  std::vector<float> second;
  resampler.Push(input.data(), 441, &second);
  resampler.Drain(&second);
  EXPECT_EQ(first.size(), 480u);
  EXPECT_TRUE(first == second);
  // DC passes at unity gain.
  EXPECT_TRUE(std::fabs(first[240] - 1.0f) < 1e-4);
}

TEST(ChannelMatrix, MonoGoesToBothFronts) {
  EXPECT_TRUE(ChannelMatrix(1, 2) == (std::vector<float>{1, 1}));
  EXPECT_TRUE(ChannelMatrix(2, 2) == (std::vector<float>{1, 0, 0, 1}));
}

TEST(ChannelMatrix, FivePointOneMixesDownWithoutClipping) {
  std::vector<float> matrix = ChannelMatrix(6, 2);
  // FL FR FC LFE BL BR: no LFE, no right channels on the left.
  EXPECT_EQ(matrix[1], 0.0f);
  EXPECT_EQ(matrix[3], 0.0f);
  EXPECT_EQ(matrix[5], 0.0f);
  EXPECT_EQ(matrix[6 + 3], 0.0f);
  EXPECT_EQ(matrix[6 + 4], 0.0f);
  float left = 0;
  float right = 0;
  for (int i = 0; i < 6; ++i) {
    left += matrix[i];
    right += matrix[6 + i];
  }
  EXPECT_TRUE(std::fabs(left - 1.0f) < 1e-6);
  EXPECT_TRUE(std::fabs(right - 1.0f) < 1e-6);
  // The center at -3 dB from the fronts.
  EXPECT_TRUE(std::fabs(matrix[2] / matrix[0] - 0.7071f) < 1e-3);
}

TEST(ChannelMatrix, StereoToMonoAverages) {
  EXPECT_TRUE(ChannelMatrix(2, 1) == (std::vector<float>{0.5f, 0.5f}));
}

}  // namespace
}  // namespace lychee
