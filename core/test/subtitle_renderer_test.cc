#include "pipeline/subtitle_renderer.h"

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

}  // namespace
}  // namespace lychee
