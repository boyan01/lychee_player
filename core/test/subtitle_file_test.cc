#include "media/subtitle_file.h"

#include "media/text_encoding.h"

#include <string>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

std::vector<SubtitleCue> Parse(std::string_view data, std::string* format) {
  std::vector<SubtitleCue> cues;
  ParseSubtitleFile(data, "", &cues, format);
  return cues;
}

TEST(SubtitleFile, SubRip) {
  std::string format;
  const auto cues = Parse(
      "1\r\n00:00:01,000 --> 00:00:02,500\r\nHello <i>world</i>\r\n\r\n"
      "2\r\n00:01:03.250 --> 00:01:04,000  X1:10\r\nTwo\r\nlines\r\n\r\n"
      // Hours past 99, and a cue ending before it starts, which goes.
      "3\n100:00:00,000 --> 100:00:01,000\nLate\n\n"
      "4\n00:00:09,000 --> 00:00:08,000\nBackwards\n",
      &format);
  EXPECT_EQ(format, std::string("subrip"));
  EXPECT_EQ(cues.size(), size_t{3});
  EXPECT_EQ(cues[0].start, int64_t{1000000});
  EXPECT_EQ(cues[0].end, int64_t{2500000});
  EXPECT_EQ(cues[0].text, std::string("Hello world"));
  EXPECT_EQ(cues[1].start, int64_t{63250000});
  EXPECT_EQ(cues[1].text, std::string("Two\nlines"));
  EXPECT_EQ(cues[2].start, int64_t{360000} * 1000000);
}

TEST(SubtitleFile, SubRipWithoutBlankLines) {
  // A timing line right after a payload starts the next cue.
  std::string format;
  const auto cues = Parse(
      "00:00:01,000 --> 00:00:02,000\nOne\n00:00:03,000 --> 00:00:04,000\nTwo",
      &format);
  EXPECT_EQ(cues.size(), size_t{2});
  EXPECT_EQ(cues[0].text, std::string("One"));
  EXPECT_EQ(cues[1].text, std::string("Two"));
}

TEST(SubtitleFile, WebVtt) {
  std::string format;
  const auto cues = Parse(
      "\xEF\xBB\xBFWEBVTT - title\nKind: captions\n\n"
      "NOTE a comment\nspanning lines\n\n"
      "intro\n00:01.000 --> 00:02.500 align:start\n<v Bob>Hello</v> "
      "&amp; <b>vtt</b>\n\n"
      "01:00:00.000 --> 01:00:01.000\nHour\n",
      &format);
  EXPECT_EQ(format, std::string("webvtt"));
  EXPECT_EQ(cues.size(), size_t{2});
  EXPECT_EQ(cues[0].start, int64_t{1000000});
  EXPECT_EQ(cues[0].end, int64_t{2500000});
  EXPECT_EQ(cues[0].text, std::string("Hello & vtt"));
  EXPECT_EQ(cues[1].start, int64_t{3600} * 1000000);
}

TEST(SubtitleFile, Ass) {
  std::string format;
  const auto cues = Parse(
      "; made by hand\n[Script Info]\nScriptType: v4.00+\n\n[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n"
      "Comment: 0,0:00:00.00,0:00:09.00,Default,,0,0,0,,not shown\n"
      "Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,{\\b1}Bold{\\b0} "
      "and, plain\\Nnext line\n"
      "Dialogue: 0,1:02:03.45,1:02:04.00,Default,,0,0,0,,Later\n",
      &format);
  EXPECT_EQ(format, std::string("ass"));
  EXPECT_EQ(cues.size(), size_t{2});
  EXPECT_EQ(cues[0].start, int64_t{1000000});
  EXPECT_EQ(cues[0].end, int64_t{2500000});
  EXPECT_EQ(cues[0].text, std::string("Bold and, plain\nnext line"));
  EXPECT_EQ(cues[1].start, int64_t{3723450000});
}

TEST(SubtitleFile, Utf16) {
  // "1\n00:00:01,000 --> 00:00:02,000\n" then U+4F60 U+597D and U+1F600.
  const std::string ascii = "1\n00:00:01,000 --> 00:00:02,000\n";
  std::string little = "\xFF\xFE";
  std::string big = "\xFE\xFF";
  for (char c : ascii) {
    little += c;
    little += '\0';
    big += '\0';
    big += c;
  }
  for (int unit : {0x4f60, 0x597d, 0xd83d, 0xde00}) {
    little += static_cast<char>(unit & 0xff);
    little += static_cast<char>(unit >> 8);
    big += static_cast<char>(unit >> 8);
    big += static_cast<char>(unit & 0xff);
  }
  for (const std::string& data : {little, big}) {
    std::string format;
    const auto cues = Parse(data, &format);
    EXPECT_EQ(cues.size(), size_t{1});
    if (!cues.empty()) {
      EXPECT_EQ(cues[0].text,
                std::string("\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x98\x80"));
    }
  }
}

// "1\n00:00:01,000 --> 00:00:02,000\n" then |text|.
std::string Cue(std::string_view text) {
  return "1\n00:00:01,000 --> 00:00:02,000\n" + std::string(text) + "\n";
}

std::string FirstText(const std::string& data, const std::string& encoding) {
  std::string format;
  std::vector<SubtitleCue> cues;
  ParseSubtitleFile(data, encoding, &cues, &format);
  return cues.empty() ? std::string() : cues[0].text;
}

TEST(SubtitleFile, LegacyEncodings) {
  // A name no platform knows reads UTF-8, invalid bytes replaced.
  EXPECT_EQ(FirstText(Cue("ok\xFF"), "no-such-encoding"),
            std::string("ok\xEF\xBF\xBD"));
  std::string probe;
  if (!ConvertToUtf8("a", "GBK", &probe)) {
    // Android without a Java VM (the tests from adb shell): no converter.
    return;
  }
  // 你好，世界 (hello, world) in GBK and Big5; à bientôt, ça va in
  // windows-1252. Guessed, as no file says its encoding.
  const std::string utf8 =
      "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C";
  EXPECT_EQ(FirstText(Cue("\xC4\xE3\xBA\xC3\xA3\xAC\xCA\xC0\xBD\xE7"), ""),
            utf8);
  EXPECT_EQ(FirstText(Cue("\xA7\x41\xA6\x6E\xA1\x41\xA5\x40\xAC\xC9"), ""),
            utf8);
  EXPECT_EQ(FirstText(Cue("\xE0 bient\xF4t, \xE7"
                          "a va"),
                      ""),
            std::string("\xC3\xA0 bient\xC3\xB4t, \xC3\xA7"
                        "a va"));
  // Named by the caller: Shift_JIS, which no guess picks (こんにちは).
  EXPECT_EQ(
      FirstText(Cue("\x82\xB1\x82\xF1\x82\xC9\x82\xBF\x82\xCD"), "Shift_JIS"),
      std::string("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1"
                  "\xE3\x81\xAF"));
}

TEST(SubtitleFile, RejectsOtherData) {
  std::string format;
  std::vector<SubtitleCue> cues;
  EXPECT_TRUE(!ParseSubtitleFile("", "", &cues, &format));
  EXPECT_TRUE(!ParseSubtitleFile("\n\n", "", &cues, &format));
  EXPECT_TRUE(!ParseSubtitleFile("<html>not found</html>", "", &cues, &format));
  EXPECT_TRUE(
      !ParseSubtitleFile("WEBVTT\n\nno cues here\n", "", &cues, &format));
  EXPECT_TRUE(!ParseSubtitleFile("a --> b\n00:61,000 --> 00:62,000\nx", "",
                                 &cues, &format));
  EXPECT_TRUE(format.empty());
}

}  // namespace
}  // namespace lychee
