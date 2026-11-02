#include "pipeline/subtitle_renderer.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

constexpr int64_t kSecond = kMicrosPerSecond;

TEST(SubtitleRenderer, ShowsCuesBetweenStartAndEnd) {
  SubtitleRenderer renderer;
  renderer.AddCue({1 * kSecond, 2 * kSecond, "a"});
  // Open-ended, closed by the empty cue at 5 s.
  renderer.AddCue({3 * kSecond, kNoTimestamp, "b"});
  renderer.AddCue({5 * kSecond, kNoTimestamp, ""});
  EXPECT_EQ(renderer.TextAt(kSecond / 2), "");
  EXPECT_EQ(renderer.TextAt(1500000), "a");
  EXPECT_EQ(renderer.TextAt(2 * kSecond), "");
  EXPECT_EQ(renderer.TextAt(4 * kSecond), "b");
  EXPECT_EQ(renderer.TextAt(5500000), "");
}

TEST(SubtitleRenderer, JoinsOverlappingCuesInStartOrder) {
  SubtitleRenderer renderer;
  renderer.AddCue({1 * kSecond, 2 * kSecond, "bottom"});
  renderer.AddCue({0, 5 * kSecond, "top"});
  EXPECT_EQ(renderer.TextAt(1500000), "top\nbottom");
  EXPECT_EQ(renderer.TextAt(3 * kSecond), "top");
}

TEST(SubtitleRenderer, IgnoresCuesReadAgain) {
  SubtitleRenderer renderer;
  renderer.AddCue({1 * kSecond, 2 * kSecond, "a"});
  renderer.AddCue({1 * kSecond, 2 * kSecond, "a"});
  EXPECT_EQ(renderer.TextAt(1500000), "a");
}

SubtitleCue BitmapCue(int64_t start, int64_t end, int width, int height) {
  SubtitleCue cue{start, end, ""};
  cue.canvas_width = 1920;
  cue.canvas_height = 1080;
  SubtitleBitmap bitmap{960, 540, width, height, {}};
  bitmap.rgba.resize(static_cast<size_t>(width) * height * 4);
  cue.bitmaps = std::make_shared<const std::vector<SubtitleBitmap>>(
      std::vector<SubtitleBitmap>{std::move(bitmap)});
  return cue;
}

// Test registration allocates before main, as the existing TESTs do.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(SubtitleRenderer, KeepsDistinctPicturesWithTheSameStart) {
  for (const int64_t second_end : {kSecond, 2 * kSecond}) {
    SubtitleRenderer renderer;
    auto first = BitmapCue(0, 2 * kSecond, 1, 1);
    auto second = BitmapCue(0, second_end, 1, 1);
    first.bitmaps = std::make_shared<const std::vector<SubtitleBitmap>>(
        std::vector<SubtitleBitmap>{{1, 1, 1, 1, {255, 0, 0, 255}}});
    second.bitmaps = std::make_shared<const std::vector<SubtitleBitmap>>(
        std::vector<SubtitleBitmap>{{2, 1, 1, 1, {0, 255, 0, 255}}});
    renderer.AddCue(first);
    renderer.AddCue(second);
    // A replay has new owners but the same complete content.
    first.bitmaps =
        std::make_shared<const std::vector<SubtitleBitmap>>(*first.bitmaps);
    second.bitmaps =
        std::make_shared<const std::vector<SubtitleBitmap>>(*second.bitmaps);
    renderer.AddCue(first);
    renderer.AddCue(second);
    auto early = renderer.ImagesAt(kSecond / 2, 0, 0, 1920, 1080);
    EXPECT_TRUE(early && early->images.size() == 2);
    if (early && early->images.size() == 2) {
      EXPECT_EQ(early->images[0].bitmap->rgba[0], 255);
      EXPECT_EQ(early->images[1].bitmap->rgba[1], 255);
    }
    auto later = renderer.ImagesAt(3 * kSecond / 2, 0, 0, 1920, 1080);
    EXPECT_TRUE(later &&
                later->images.size() == (second_end == kSecond ? 1u : 2u));
    EXPECT_TRUE(renderer.ImagesAt(2 * kSecond, 0, 0, 1920, 1080) == nullptr);
  }
}

TEST(SubtitleRenderer, ScalesPicturesToTheCanvas) {
  SubtitleRenderer renderer;
  renderer.AddCue(BitmapCue(1 * kSecond, 2 * kSecond, 200, 100));
  EXPECT_EQ(renderer.ImagesKeyAt(kSecond / 2), 0u);
  EXPECT_TRUE(renderer.ImagesAt(kSecond / 2, 0, 0, 1920, 1080) == nullptr);
  EXPECT_TRUE(renderer.ImagesKeyAt(1500000) != 0u);
  // On a 960x540 canvas: half the size, half the position.
  auto images = renderer.ImagesAt(1500000, 960, 540, 1920, 1080);
  EXPECT_TRUE(images != nullptr);
  EXPECT_EQ(images->images.size(), 1u);
  EXPECT_EQ(images->images[0].x, 480);
  EXPECT_EQ(images->images[0].y, 270);
  EXPECT_EQ(images->images[0].width, 100);
  EXPECT_EQ(images->images[0].height, 50);
  EXPECT_EQ(images->images[0].bitmap->width, 200);
  // The same object while nothing changes; a new id for a new canvas.
  EXPECT_TRUE(renderer.ImagesAt(1600000, 960, 540, 1920, 1080) == images);
  auto full = renderer.ImagesAt(1600000, 0, 0, 1920, 1080);
  EXPECT_TRUE(full->id != images->id);
  EXPECT_EQ(full->images[0].width, 200);
  EXPECT_TRUE(renderer.ImagesAt(2 * kSecond, 0, 0, 1920, 1080) == nullptr);
}

TEST(SubtitleRenderer, DropsPicturesFurthestFromTheLatest) {
  SubtitleRenderer renderer;
  // 8 MB each: the fifth goes over the limit, so the first one goes.
  for (int i = 0; i < 5; ++i) {
    renderer.AddCue(BitmapCue(i * kSecond, (i + 1) * kSecond, 2048, 1024));
  }
  EXPECT_EQ(renderer.ImagesKeyAt(kSecond / 2), 0u);
  EXPECT_TRUE(renderer.ImagesKeyAt(1500000) != 0u);
  EXPECT_TRUE(renderer.ImagesKeyAt(4500000) != 0u);
}

// Test registration allocates before main, as the existing TESTs do.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(SubtitleRenderer, DroppedPicturesLeaveRoomForNewOnes) {
  SubtitleRenderer renderer;
  // 8 MB each, 32 MB in all: at the limit, not over it.
  for (int i = 0; i < 4; ++i) {
    renderer.AddCue(BitmapCue(i * kSecond, (i + 1) * kSecond, 2048, 1024));
  }
  renderer.DropEndedBy(10 * kSecond);
  EXPECT_EQ(renderer.ImagesKeyAt(kSecond / 2), 0u);
  for (int i = 20; i < 24; ++i) {
    renderer.AddCue(BitmapCue(i * kSecond, (i + 1) * kSecond, 2048, 1024));
  }
  EXPECT_TRUE(renderer.ImagesKeyAt(20500000) != 0u);
}

TEST(AssToPlainText, StripsFieldsAndMarkup) {
  EXPECT_EQ(AssToPlainText("Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,"
                           "0,,{\\b1}Bold{\\b0} and, plain\\Nnext line"),
            "Bold and, plain\nnext line");
  // FFmpeg's decoded events and Matroska packets: ReadOrder first, no times.
  EXPECT_EQ(AssToPlainText("3,0,Default,,0,0,0,,Hello {\\i1}world{\\i0}"),
            "Hello world");
  EXPECT_EQ(AssToPlainText("0,0,Default,,0,0,0,,a\\hb\\n"), "a b");
  EXPECT_EQ(AssToPlainText("no fields"), "");
}

// The text showing at |time| by the rules, from every cue.
std::string ReferenceTextAt(std::vector<SubtitleCue> cues, int64_t time) {
  std::stable_sort(cues.begin(), cues.end(),
                   [](const SubtitleCue& a, const SubtitleCue& b) {
                     return a.start < b.start;
                   });
  std::string text;
  for (const SubtitleCue& cue : cues) {
    if (cue.start > time || cue.text.empty()) {
      continue;
    }
    int64_t end = cue.end;
    if (end == kNoTimestamp) {
      end = kInfiniteTime;
      for (const SubtitleCue& next : cues) {
        if (next.start > cue.start) {
          end = next.start;
          break;
        }
      }
    }
    if (time < end) {
      text += (text.empty() ? "" : "\n") + cue.text;
    }
  }
  return text;
}

// Test registration allocates before main, as the existing TESTs do.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(SubtitleRenderer, LooksUpManyCuesAddedInAnyOrder) {
  // Short and long cues, overlapping, some open-ended or empty, added out
  // of order with lookups in between, as cues read again after seeks.
  // A fixed seed, so a failure repeats.
  std::mt19937 random(7);  // NOLINT(bugprone-random-generator-seed)
  SubtitleRenderer renderer;
  std::vector<SubtitleCue> cues;
  int failures = 0;
  for (int i = 0; i < 400 && failures == 0; ++i) {
    const int64_t start =
        static_cast<int64_t>(random() % 2000) * kMicrosPerMilli * 50;
    int64_t end = start + static_cast<int64_t>(random() % 6000) * 1000;
    if (random() % 8 == 0) {
      end = kNoTimestamp;
    } else if (random() % 40 == 0) {
      end = start + 300 * kSecond;
    }
    SubtitleCue cue{start, end,
                    random() % 10 == 0 ? "" : "cue " + std::to_string(i)};
    renderer.AddCue(cue);
    cues.push_back(cue);
    for (int j = 0; j < 20; ++j) {
      const int64_t time = static_cast<int64_t>(random() % 101000) * 1000;
      const std::string expected = ReferenceTextAt(cues, time);
      if (renderer.TextAt(time) != expected) {
        EXPECT_EQ(renderer.TextAt(time), expected);
        failures++;
        break;
      }
    }
  }
}

// Test registration allocates before main, as the existing TESTs do.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(SubtitleRenderer, DropsCuesEndedAsLiveInputPlays) {
  // Cues arrive in start order, as from a live stream, while playback
  // follows a few seconds behind, dropping what ended before it.
  std::mt19937 random(11);  // NOLINT(bugprone-random-generator-seed)
  SubtitleRenderer renderer;
  std::vector<SubtitleCue> cues;
  int64_t start = 0;
  int failures = 0;
  for (int i = 0; i < 400 && failures == 0; ++i) {
    start += static_cast<int64_t>(random() % 3000) * kMicrosPerMilli;
    int64_t end = start + static_cast<int64_t>(random() % 6000) * 1000;
    if (random() % 8 == 0) {
      end = kNoTimestamp;
    } else if (random() % 40 == 0) {
      end = start + 30 * kSecond;
    }
    SubtitleCue cue{start, end,
                    random() % 10 == 0 ? "" : "cue " + std::to_string(i)};
    renderer.AddCue(cue);
    cues.push_back(cue);
    const int64_t playing = start - 5 * kSecond;
    renderer.DropEndedBy(playing);
    for (int j = 0; j < 5; ++j) {
      const int64_t time =
          playing + static_cast<int64_t>(random() % 8000) * 1000;
      const std::string expected = ReferenceTextAt(cues, time);
      if (renderer.TextAt(time) != expected) {
        EXPECT_EQ(renderer.TextAt(time), expected);
        failures++;
        break;
      }
    }
  }
  // Long before the time dropped by, what showed is gone.
  for (const SubtitleCue& cue : cues) {
    if (!ReferenceTextAt(cues, cue.start).empty()) {
      EXPECT_EQ(renderer.TextAt(cue.start), "");
      break;
    }
  }
}

}  // namespace
}  // namespace lychee
