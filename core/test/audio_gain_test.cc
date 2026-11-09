#include "pipeline/audio_gain.h"

#include <cmath>
#include <vector>

#include "pipeline/audio_tap.h"
#include "test.h"

namespace lychee {
namespace {

bool Near(float a, float b) {
  return std::abs(a - b) < 1e-5f;
}

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)

TEST(LevelRamp, StepsEvenlyToTarget) {
  LevelRamp ramp(1.0f);
  ramp.Set(0.0f, 4);
  EXPECT_TRUE(Near(ramp.Next(), 0.75f));
  EXPECT_TRUE(Near(ramp.Next(), 0.5f));
  EXPECT_TRUE(Near(ramp.Next(), 0.25f));
  EXPECT_EQ(ramp.Next(), 0.0f);
  EXPECT_EQ(ramp.Next(), 0.0f);
}

TEST(LevelRamp, ZeroFramesJumps) {
  LevelRamp ramp(1.0f);
  ramp.Set(0.3f, 0);
  EXPECT_EQ(ramp.Next(), 0.3f);
}

TEST(LevelRamp, NewTargetGoesOnFromWhereItIs) {
  LevelRamp ramp(0.0f);
  ramp.Set(1.0f, 10);
  for (int i = 0; i < 5; ++i) {
    ramp.Next();
  }
  ramp.Set(0.0f, 5);
  // 0.5 down to 0 in five steps: no jump.
  EXPECT_TRUE(Near(ramp.Next(), 0.4f));
}

TEST(LevelRamp, LongRampEndsExactlyOnTarget) {
  LevelRamp ramp(1.0f);
  ramp.Set(0.123f, 480000);
  float last = 1.0f;
  float largest_step = 0;
  for (int i = 0; i < 480000; ++i) {
    const float level = ramp.Next();
    largest_step = std::max(largest_step, std::abs(level - last));
    last = level;
  }
  EXPECT_EQ(last, 0.123f);
  EXPECT_TRUE(largest_step < 1e-5f);
}

TEST(LevelRamp, SkipAdvances) {
  LevelRamp ramp(0.0f);
  ramp.Set(1.0f, 10);
  ramp.Skip(4);
  EXPECT_TRUE(Near(ramp.Next(), 0.5f));
  ramp.Skip(100);
  EXPECT_EQ(ramp.Next(), 1.0f);
}

TEST(AudioGain, VolumeTimesGain) {
  AudioGain gain;
  gain.SetVolume(0.5f, 0);
  gain.SetGain(0.5f, 0, 0);
  EXPECT_EQ(gain.Next(), 0.25f);
}

TEST(AudioGain, BoostLimitedByGivenPeak) {
  AudioGain gain;
  gain.SetGain(8.0f, 0.25f, 0);
  EXPECT_EQ(gain.gain(), 4.0f);
  // A louder sample decoded lowers it further.
  gain.NotePeak(0.5f, 0);
  EXPECT_EQ(gain.gain(), 2.0f);
  // A quieter one does not raise it again.
  gain.NotePeak(0.1f, 0);
  EXPECT_EQ(gain.gain(), 2.0f);
}

TEST(AudioGain, BoostLimitRampsDown) {
  AudioGain gain;
  gain.SetGain(4.0f, 0, 0);
  EXPECT_EQ(gain.Next(), 4.0f);
  gain.NotePeak(0.5f, 4);
  EXPECT_TRUE(Near(gain.Next(), 3.5f));
  gain.Skip(3);
  EXPECT_EQ(gain.Next(), 2.0f);
}

TEST(AudioGain, NeverCutsBelowUnityOrChangesACut) {
  AudioGain gain;
  // A decoder overshooting full scale does not turn a boost into a cut.
  gain.SetGain(2.0f, 0, 0);
  gain.NotePeak(1.5f, 0);
  EXPECT_EQ(gain.gain(), 1.0f);
  gain.SetGain(0.5f, 0.9f, 0);
  EXPECT_EQ(gain.gain(), 0.5f);
}

TEST(AudioGain, DecodedPeakOutlivesNewGain) {
  AudioGain gain;
  gain.NotePeak(0.5f, 0);
  gain.SetGain(4.0f, 0, 0);
  EXPECT_EQ(gain.gain(), 2.0f);
}

TEST(AudioGain, VolumeClamped) {
  AudioGain gain;
  gain.SetVolume(3.0f, 0);
  EXPECT_EQ(gain.volume(), 1.0f);
  gain.SetVolume(std::nanf(""), 0);
  EXPECT_EQ(gain.volume(), 0.0f);
}

constexpr int kRate = 1000;  // A frame per millisecond.
constexpr int64_t kMs = kMicrosPerMilli;

// Writes |frames| stereo frames whose mono mix counts up from |first|.
void WriteCount(AudioTap* tap, float first, int frames, int64_t at) {
  std::vector<float> samples;
  for (int i = 0; i < frames; ++i) {
    samples.push_back(first + static_cast<float>(i) - 1);
    samples.push_back(first + static_cast<float>(i) + 1);
  }
  tap->Write(samples.data(), 2, frames, frames, at);
}

TEST(AudioTap, ReadsWhatPlayedUpToTime) {
  AudioTap tap;
  tap.Initialize(kRate, 100);
  WriteCount(&tap, 0, 10, 1000 * kMs);
  WriteCount(&tap, 10, 10, 1010 * kMs);
  float out[4];
  // Heard up to 1012 ms: frames 8 to 11.
  tap.Read(1012 * kMs, out, 4);
  EXPECT_EQ(out[0], 8.0f);
  EXPECT_EQ(out[3], 11.0f);
  // Not yet played, and before the first: silence.
  tap.Read(1022 * kMs, out, 4);
  EXPECT_EQ(out[0], 18.0f);
  EXPECT_EQ(out[1], 19.0f);
  EXPECT_EQ(out[2], 0.0f);
  tap.Read(1001 * kMs, out, 4);
  EXPECT_EQ(out[2], 0.0f);
  EXPECT_EQ(out[3], 0.0f);
}

TEST(AudioTap, SilenceAfterWrittenAndInGaps) {
  AudioTap tap;
  tap.Initialize(kRate, 100);
  std::vector<float> samples(20, 1.0f);
  // Five frames of audio, five the device fills with silence.
  tap.Write(samples.data(), 2, 5, 10, 0);
  // Paused for 10 ms.
  tap.Write(samples.data(), 2, 10, 10, 20 * kMs);
  float out[30];
  tap.Read(30 * kMs, out, 30);
  EXPECT_EQ(out[4], 1.0f);
  EXPECT_EQ(out[5], 0.0f);
  EXPECT_EQ(out[19], 0.0f);
  EXPECT_EQ(out[20], 1.0f);
  EXPECT_EQ(out[29], 1.0f);
}

TEST(AudioTap, KeepsOnlyItsCapacity) {
  AudioTap tap;
  tap.Initialize(kRate, 10);
  WriteCount(&tap, 0, 25, 0);
  float out[12];
  tap.Read(25 * kMs, out, 12);
  EXPECT_EQ(out[0], 0.0f);
  EXPECT_EQ(out[1], 0.0f);
  EXPECT_EQ(out[2], 15.0f);
  EXPECT_EQ(out[11], 24.0f);
}

TEST(AudioTap, JitterKeepsTimeline) {
  AudioTap tap;
  tap.Initialize(kRate, 100);
  WriteCount(&tap, 0, 10, 0);
  // Reported 1 ms late: still the next frames, on the first's timeline.
  WriteCount(&tap, 10, 10, 11 * kMs);
  float out[1];
  tap.Read(20 * kMs, out, 1);
  EXPECT_EQ(out[0], 19.0f);
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
