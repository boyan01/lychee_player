#include "pipeline/video_renderer_algorithm.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "pipeline/video_cadence_estimator.h"
#include "test.h"

namespace lychee {
namespace {

constexpr int64_t kSecond = kMicrosPerSecond;

std::string CadenceFor(double fps, double hz) {
  VideoCadenceEstimator estimator(8 * kSecond);
  const int64_t frame_duration = std::llround(kSecond / fps);
  const int64_t render_interval = std::llround(kSecond / hz);
  const int64_t max_drift = std::clamp<int64_t>(
      frame_duration / 2, kSecond / 60, kSecond / 8);
  // The estimate must hold for 100 ms before it is accepted.
  for (int i = 0; i < 20; ++i) {
    estimator.UpdateCadenceEstimate(render_interval, frame_duration, 0,
                                    max_drift);
  }
  if (!estimator.has_cadence()) {
    return "[]";
  }
  // Two periods of the pattern, which is at most 5 long.
  std::string result;
  for (int i = 0; i < 10; ++i) {
    result += std::to_string(estimator.GetCadenceForFrame(i));
  }
  return result;
}

TEST(VideoCadenceEstimator, StandardRates) {
  EXPECT_EQ(CadenceFor(30, 60), "2222222222");
  EXPECT_EQ(CadenceFor(60, 60), "1111111111");
  EXPECT_EQ(CadenceFor(24, 60), "3232323232");
  EXPECT_EQ(CadenceFor(25, 50), "2222222222");
  EXPECT_EQ(CadenceFor(29.97, 60), "2222222222");
  EXPECT_EQ(CadenceFor(120, 60), "1010101010");
  EXPECT_EQ(CadenceFor(30, 144), "5545555455");
  EXPECT_EQ(CadenceFor(1, 60), "[]");
}

TEST(VideoCadenceEstimator, VariableFrameRateHasNoCadence) {
  VideoCadenceEstimator estimator(8 * kSecond);
  for (int i = 0; i < 20; ++i) {
    estimator.UpdateCadenceEstimate(16667, 33333, 12000, 16667);
  }
  EXPECT_TRUE(!estimator.has_cadence());
}

// Plays |fps| content at |hz| for |seconds|. Counts how many intervals each
// frame was shown for, and drops once the cadence had time to settle (it
// must hold 100 ms; frames skipped before that count as dropped).
struct PlaybackResult {
  std::map<int, int> intervals_per_frame;
  size_t dropped = 0;
};

PlaybackResult Play(double fps, double hz, int seconds) {
  constexpr int64_t kBase = 1000 * kSecond;
  VideoRendererAlgorithm algorithm(
      [](const std::vector<int64_t>& media, std::vector<int64_t>* wall) {
        wall->clear();
        for (int64_t t : media) {
          wall->push_back(kBase + t);
        }
        return true;
      });
  const int64_t frame_duration = std::llround(kSecond / fps);
  const int64_t interval = std::llround(kSecond / hz);
  int64_t next_pts = 0;
  std::map<int64_t, int> shown;
  PlaybackResult result;
  for (int64_t t = 0; t < seconds * kSecond; t += interval) {
    // Keep 200 ms queued, like the decode thread does.
    while (next_pts < t + 200000) {
      auto frame = std::make_shared<VideoFrame>();
      frame->pts = next_pts;
      frame->duration = frame_duration;
      algorithm.EnqueueFrame(frame);
      next_pts += frame_duration;
    }
    size_t dropped = 0;
    auto frame = algorithm.Render(kBase + t, kBase + t + interval, &dropped);
    if (t >= 200000) {
      result.dropped += dropped;
    }
    ++shown[frame->pts];
  }
  // Skip the first and last frames: startup and the cut-off end.
  shown.erase(shown.begin());
  shown.erase(std::prev(shown.end()));
  for (const auto& [pts, count] : shown) {
    ++result.intervals_per_frame[count];
  }
  return result;
}

TEST(VideoRendererAlgorithm, ThirtyAtSixtyShowsEachFrameTwice) {
  PlaybackResult result = Play(30, 60, 5);
  EXPECT_EQ(result.dropped, 0u);
  EXPECT_EQ(result.intervals_per_frame.size(), 1u);
  EXPECT_EQ(result.intervals_per_frame.begin()->first, 2);
}

TEST(VideoRendererAlgorithm, TwentyFourAtSixtyAlternatesThreeTwo) {
  PlaybackResult result = Play(24, 60, 5);
  EXPECT_EQ(result.dropped, 0u);
  EXPECT_EQ(result.intervals_per_frame.size(), 2u);
  EXPECT_NEAR(result.intervals_per_frame[3], result.intervals_per_frame[2], 1);
}

TEST(VideoRendererAlgorithm, SixtyAtSixtyShowsEveryFrame) {
  PlaybackResult result = Play(60, 60, 5);
  EXPECT_EQ(result.dropped, 0u);
  EXPECT_EQ(result.intervals_per_frame.size(), 1u);
  EXPECT_EQ(result.intervals_per_frame.begin()->first, 1);
}

TEST(VideoRendererAlgorithm, SixtyAtThirtySkipsEveryOtherFrame) {
  PlaybackResult result = Play(60, 30, 5);
  // Frames a cadence skips are not drops.
  EXPECT_EQ(result.dropped, 0u);
}

TEST(VideoRendererAlgorithm, ResetEmptiesQueue) {
  VideoRendererAlgorithm algorithm(
      [](const std::vector<int64_t>& media, std::vector<int64_t>* wall) {
        *wall = media;
        return true;
      });
  auto frame = std::make_shared<VideoFrame>();
  frame->pts = 0;
  algorithm.EnqueueFrame(frame);
  EXPECT_EQ(algorithm.frames_queued(), 1u);
  algorithm.Reset();
  EXPECT_EQ(algorithm.frames_queued(), 0u);
}

TEST(VideoRendererAlgorithm, DropsFramesAtOrBeforeRenderedOne) {
  VideoRendererAlgorithm algorithm(
      [](const std::vector<int64_t>& media, std::vector<int64_t>* wall) {
        *wall = media;
        return true;
      });
  for (int64_t pts : {0, 33333, 66667}) {
    auto frame = std::make_shared<VideoFrame>();
    frame->pts = pts;
    frame->duration = 33333;
    algorithm.EnqueueFrame(frame);
  }
  size_t dropped = 0;
  algorithm.Render(40000, 56667, &dropped);
  auto late = std::make_shared<VideoFrame>();
  late->pts = 10000;
  algorithm.EnqueueFrame(late);
  algorithm.Render(56667, 73333, &dropped);
  EXPECT_TRUE(dropped >= 1u);
}

}  // namespace
}  // namespace lychee
