#include "backends/mp4/mp4_subtitle.h"

#include <string>
#include <vector>

#include "test.h"

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)

namespace lychee {
namespace {

std::vector<SubtitleCue> Ttml(const std::string& body,
                              int64_t start = 0,
                              int64_t duration = 10000000) {
  std::vector<SubtitleCue> cues;
  EXPECT_TRUE(ParseMp4Subtitle("stpp", body, start, duration, &cues));
  return cues;
}

TEST(Mp4Subtitle, ImscTextTimingAndEntities) {
  auto cues = Ttml(
      "<?xml version='1.0'?><tt xmlns='http://www.w3.org/ns/ttml'>"
      "<head><metadata>not a cue</metadata></head><body begin='1s'>"
      "<div begin='500ms'><p begin='00:00:00.500' end='3s'>"
      " Hello <span>world &amp; &#x1f600;</span><br/>Next &lt;line&gt;"
      "</p></div></body></tt>");
  EXPECT_EQ(cues.size(), 1u);
  if (cues.size() == 1) {
    EXPECT_EQ(cues[0].start, 2000000);
    EXPECT_EQ(cues[0].end, 4500000);
    EXPECT_EQ(cues[0].text, "Hello world & \xf0\x9f\x98\x80\nNext <line>");
  }
}

TEST(Mp4Subtitle, TimedSpansClipToSampleAndParent) {
  auto cues = Ttml(
      "<tt><body><div><p begin='1s' end='5s'>A"
      "<span begin='1s' dur='1s'> B</span> C</p>"
      "</div></body></tt>",
      2500000, 2000000);
  EXPECT_EQ(cues.size(), 2u);
  if (cues.size() == 2) {
    EXPECT_EQ(cues[0].start, 2500000);
    EXPECT_EQ(cues[0].end, 3000000);
    EXPECT_EQ(cues[0].text, "A B C");
    EXPECT_EQ(cues[1].start, 3000000);
    EXPECT_EQ(cues[1].end, 4500000);
    EXPECT_EQ(cues[1].text, "A C");
  }
}

TEST(Mp4Subtitle, NamespaceFrameAndTickTimes) {
  auto cues = Ttml(
      "<tt:tt xmlns:tt='http://www.w3.org/ns/ttml' "
      "ttp:frameRate='25' ttp:subFrameRate='10' ttp:tickRate='100'>"
      "<tt:body><tt:p begin='00:00:01:12.5' dur='25t' xml:space='preserve'>"
      " x  y </tt:p><tt:p begin='50f' dur='100ms'><![CDATA[A < B]]>"
      "</tt:p></tt:body></tt:tt>");
  EXPECT_EQ(cues.size(), 2u);
  if (cues.size() == 2) {
    EXPECT_EQ(cues[0].start, 1500000);
    EXPECT_EQ(cues[0].end, 1750000);
    EXPECT_EQ(cues[1].start, 2000000);
    EXPECT_EQ(cues[1].text, "A < B");
  }
}

TEST(Mp4Subtitle, RejectsMalformedAndUnboundedXml) {
  const char* invalid[] = {
      "<!DOCTYPE tt [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><tt/>",
      "<tt><body><p>&unknown;</p></body></tt>",
      "<tt><body><p begin='NaNs'>x</p></body></tt>",
      "<tt><body><p begin='999999999999999999999999999s'>x</p></body></tt>",
      "<tt><body><p>x</body></p></tt>",
      "<tt><body><p>x</p></body></tt><tt/>",
      "<tt ttp:frameRate='0'><body/></tt>",
  };
  for (const char* sample : invalid) {
    std::vector<SubtitleCue> cues;
    EXPECT_TRUE(!ParseMp4Subtitle("stpp", sample, 0, 1000000, &cues));
    EXPECT_TRUE(cues.empty());
  }
}

std::string Box(const char* name, const std::string& payload) {
  std::string box;
  const uint32_t size = static_cast<uint32_t>(payload.size() + 8);
  for (int i = 3; i >= 0; --i) {
    box += static_cast<char>(size >> (i * 8));
  }
  box.append(name, 4);
  return box + payload;
}

TEST(Mp4Subtitle, BoxedWebVttMultipleCuesAndEmptySample) {
  const std::string sample =
      Box("vttc", Box("iden", "id") + Box("payl", "<b>Hello</b> &amp; bye")) +
      Box("vttc", Box("payl", "Second"));
  std::vector<SubtitleCue> cues;
  EXPECT_TRUE(ParseMp4Subtitle("wvtt", sample, 2000000, 1500000, &cues));
  EXPECT_EQ(cues.size(), 2u);
  if (cues.size() == 2) {
    EXPECT_EQ(cues[0].start, 2000000);
    EXPECT_EQ(cues[0].end, 3500000);
    EXPECT_EQ(cues[0].text, "Hello & bye");
    EXPECT_EQ(cues[1].text, "Second");
  }
  cues.clear();
  EXPECT_TRUE(ParseMp4Subtitle("wvtt", Box("vtte", ""), 0, 1000000, &cues));
  EXPECT_TRUE(cues.empty());
  EXPECT_TRUE(!ParseMp4Subtitle("wvtt", sample.substr(0, sample.size() - 1), 0,
                                1000000, &cues));
}

}  // namespace
}  // namespace lychee

// NOLINTEND(bugprone-throwing-static-initialization)
