#include "pipeline/audio_renderer_algorithm.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int64_t kMs = kMicrosPerMilli;

// |frames| of a |frequency| Hz tone, continuing from |first_frame|.
std::shared_ptr<AudioBuffer> Tone(double frequency,
                                  int first_frame,
                                  int frames) {
  auto buffer = std::make_shared<AudioBuffer>();
  buffer->pts =
      static_cast<int64_t>(first_frame) * kMicrosPerSecond / kSampleRate;
  buffer->sample_rate = kSampleRate;
  buffer->channels = kChannels;
  buffer->samples.resize(static_cast<size_t>(frames) * kChannels);
  for (int i = 0; i < frames; ++i) {
    const float value = 0.5f * static_cast<float>(std::sin(
                                   2 * std::numbers::pi * frequency *
                                   (first_frame + i) / kSampleRate));
    for (int k = 0; k < kChannels; ++k) {
      buffer->samples[i * kChannels + k] = value;
    }
  }
  return buffer;
}

// Enqueues |seconds| of a 440 Hz tone in 10 ms buffers.
void EnqueueTone(AudioRendererAlgorithm* algorithm, double seconds) {
  const int chunk = kSampleRate / 100;
  for (int frame = 0; frame < seconds * kSampleRate; frame += chunk) {
    algorithm->EnqueueBuffer(Tone(440, frame, chunk));
  }
}

// Pulls 512-frame blocks at |rate| until no more come; returns channel 0.
std::vector<float> Drain(AudioRendererAlgorithm* algorithm, double rate) {
  AudioBus bus(kChannels, 512);
  std::vector<float> output;
  for (;;) {
    const int frames = algorithm->FillBuffer(&bus, 0, 512, rate);
    if (frames == 0) {
      return output;
    }
    output.insert(output.end(), bus.channel(0), bus.channel(0) + frames);
  }
}

// Rising zero crossings per second: the tone's frequency.
double Frequency(const std::vector<float>& samples) {
  int crossings = 0;
  for (size_t i = 1; i < samples.size(); ++i) {
    crossings += samples[i - 1] < 0 && samples[i] >= 0;
  }
  return crossings * static_cast<double>(kSampleRate) / samples.size();
}

TEST(AudioRendererAlgorithm, PassesNormalRateThrough) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  auto tone = Tone(440, 0, 4800);
  algorithm.EnqueueBuffer(tone);
  AudioBus bus(kChannels, 4800);
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 4800, 1.0), 4800);
  EXPECT_TRUE(algorithm.last_mode() ==
              AudioRendererAlgorithm::FillBufferMode::kPassthrough);
  bool same = true;
  for (int i = 0; i < 4800; ++i) {
    same = same && bus.channel(1)[i] == tone->samples[i * kChannels + 1];
  }
  EXPECT_TRUE(same);
  EXPECT_EQ(algorithm.NextOutputTimestamp(), 100 * kMs);
}

TEST(AudioRendererAlgorithm, FasterRateHalvesDurationAndKeepsPitch) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  EnqueueTone(&algorithm, 1);
  std::vector<float> output = Drain(&algorithm, 2.0);
  EXPECT_TRUE(algorithm.last_mode() ==
              AudioRendererAlgorithm::FillBufferMode::kWSOLA);
  // Up to a search block of input stays behind.
  EXPECT_NEAR(static_cast<long long>(output.size()), 24000LL, 2400LL);
  EXPECT_NEAR(std::llround(Frequency(output)), 440LL, 15LL);
}

TEST(AudioRendererAlgorithm, SlowerRateDoublesDurationAndKeepsPitch) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  EnqueueTone(&algorithm, 1);
  std::vector<float> output = Drain(&algorithm, 0.5);
  EXPECT_NEAR(static_cast<long long>(output.size()), 96000LL, 4800LL);
  EXPECT_NEAR(std::llround(Frequency(output)), 440LL, 15LL);
}

TEST(AudioRendererAlgorithm, OutputTimestampFollowsRate) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  EnqueueTone(&algorithm, 1);
  AudioBus bus(kChannels, 4800);
  // 100 ms of output at 2x is 200 ms of media.
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 4800, 2.0), 4800);
  EXPECT_NEAR(algorithm.NextOutputTimestamp(), 200 * kMs, 30 * kMs);
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 4800, 2.0), 4800);
  EXPECT_NEAR(algorithm.NextOutputTimestamp(), 400 * kMs, 30 * kMs);
}

TEST(AudioRendererAlgorithm, FlushDropsInput) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  EnqueueTone(&algorithm, 0.5);
  AudioBus bus(kChannels, 512);
  algorithm.FillBuffer(&bus, 0, 512, 1.5);
  algorithm.FlushBuffers();
  EXPECT_EQ(algorithm.BufferedFrames(), 0);
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 512, 1.5), 0);
  EXPECT_EQ(algorithm.NextOutputTimestamp(), kNoTimestamp);
}

TEST(AudioRendererAlgorithm, EndInputAtCutsPastTheBoundary) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  // 10 ms buffers to 105 ms, partly read: the loop boundary is at 52 ms.
  EnqueueTone(&algorithm, 0.1);
  algorithm.EnqueueBuffer(Tone(440, 4800, 240));
  AudioBus bus(kChannels, 960);
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 960, 1.0), 960);
  algorithm.EndInputAt(52 * kMs);
  EXPECT_EQ(algorithm.BufferedFrames(), 52 * 48 - 960);
  // The next pass continues the timeline.
  algorithm.EnqueueBuffer(Tone(440, 52 * 48, 480));
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 900, 1.0), 900);
  EXPECT_EQ(algorithm.NextOutputTimestamp(), 38750);
  // Before what was read: nothing is left, and time does not go back.
  algorithm.EndInputAt(30 * kMs);
  EXPECT_EQ(algorithm.BufferedFrames(), 0);
  EXPECT_EQ(algorithm.NextOutputTimestamp(), 38750);
}

TEST(AudioRendererAlgorithm, EndInputAtPadsAShortTrackWithSilence) {
  AudioRendererAlgorithm algorithm;
  algorithm.Initialize(kChannels, kSampleRate);
  algorithm.EnqueueBuffer(Tone(440, 0, 480));
  algorithm.EndInputAt(25 * kMs);
  EXPECT_EQ(algorithm.BufferedFrames(), 25 * 48);
  AudioBus bus(kChannels, 25 * 48);
  EXPECT_EQ(algorithm.FillBuffer(&bus, 0, 25 * 48, 1.0), 25 * 48);
  EXPECT_TRUE(bus.channel(0)[379] != 0.0f && bus.channel(0)[600] == 0.0f &&
              bus.channel(1)[25 * 48 - 1] == 0.0f);
  EXPECT_EQ(algorithm.NextOutputTimestamp(), 25 * kMs);
}

TEST(Wsola, QuadraticInterpolationFindsPeak) {
  float extremum;
  float value;
  const float symmetric[3] = {1, 2, 1};
  wsola::QuadraticInterpolation(symmetric, &extremum, &value);
  EXPECT_TRUE(extremum == 0 && value == 2);
  const float skewed[3] = {0, 1, 0.5f};
  wsola::QuadraticInterpolation(skewed, &extremum, &value);
  EXPECT_TRUE(std::fabs(extremum - 1.0f / 6) < 1e-5f);
}

TEST(Wsola, HanningWindowOverlapAddsToOne) {
  std::vector<float> window(960);
  wsola::GetPeriodicHanningWindow(&window);
  EXPECT_TRUE(window[0] == 0 && std::fabs(window[480] - 1) < 1e-6f);
  bool flat = true;
  for (int n = 0; n < 480; ++n) {
    flat = flat && std::fabs(window[n] + window[n + 480] - 1) < 1e-5f;
  }
  EXPECT_TRUE(flat);
}

TEST(Wsola, OptimalIndexFindsCopiedBlock) {
  // Low frequencies, so the decimated search sees the match coming.
  auto signal = [](double a, double b, int n) {
    return static_cast<float>(std::sin(a * n) + 0.6 * std::sin(b * n + 1));
  };
  AudioBus search(1, 1500);
  AudioBus target(1, 200);
  for (int n = 0; n < 1500; ++n) {
    search.channel(0)[n] = signal(0.021, 0.047, n);
  }
  for (int n = 0; n < 200; ++n) {
    target.channel(0)[n] = signal(0.033, 0.013, n);
    search.channel(0)[612 + n] = target.channel(0)[n];
  }
  EXPECT_EQ(wsola::OptimalIndex(search, target, {-1000, -900}), 612);
  // Excluded, it must pick something else.
  EXPECT_TRUE(wsola::OptimalIndex(search, target, {600, 620}) != 612);
}

}  // namespace
}  // namespace lychee
