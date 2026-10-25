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
