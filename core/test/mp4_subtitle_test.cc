#include "backends/mp4/mp4_subtitle.h"

#include <string>
#include <vector>

#include "media/factory.h"
#include "media/subtitle_image.h"
#include "pipeline/subtitle_renderer.h"
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

std::vector<uint8_t> ImagePng() {
  return {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
          0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02,
          0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
          0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x15, 0x49, 0x44, 0x41,
          0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x08,
          0x1b, 0x18, 0x80, 0xf4, 0x7f, 0x10, 0x0b, 0x00, 0x40, 0xd6,
          0x07, 0x7a, 0x78, 0xd2, 0x6e, 0xe1, 0x00, 0x00, 0x00, 0x00,
          0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
}

TEST(Mp4Subtitle, DecodesIndexedAndSixteenBitPngRasters) {
  // Two rows: red/blue, blue/red. Indexed one-bit samples are packed.
  const std::vector<uint8_t> indexed = {
      137, 80,  78,  71,  13,  10,  26, 10, 0,   0,  0,   13,  73,  72, 68,
      82,  0,   0,   0,   2,   0,   0,  0,  2,   1,  3,   0,   0,   0,  72,
      120, 159, 103, 0,   0,   0,   6,  80, 76,  84, 69,  255, 0,   0,  0,
      0,   255, 108, 161, 253, 142, 0,  0,  0,   12, 73,  68,  65,  84, 120,
      156, 99,  112, 96,  104, 0,   0,  1,  68,  0,  193, 58,  122, 28, 86,
      0,   0,   0,   0,   73,  69,  78, 68, 174, 66, 96,  130};
  // Big-endian grayscale: black/white, white/black.
  const std::vector<uint8_t> gray = {
      137, 80,  78,  71, 13,  10, 26, 10, 0,   0,  0,   13,  73,  72,  68,
      82,  0,   0,   0,  2,   0,  0,  0,  2,   16, 0,   0,   0,   0,   7,
      77,  142, 187, 0,  0,   0,  15, 73, 68,  65, 84,  120, 156, 99,  96,
      96,  248, 255, 31, 132, 24, 0,  19, 246, 3,  253, 148, 231, 170, 71,
      0,   0,   0,   0,  73,  69, 78, 68, 174, 66, 96,  130};
  SubtitleBitmap bitmap;
  EXPECT_TRUE(DecodeSubtitlePng(indexed, &bitmap));
  EXPECT_EQ(bitmap.width, 2);
  EXPECT_EQ(bitmap.height, 2);
  EXPECT_TRUE(bitmap.rgba ==
              std::vector<uint8_t>({255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255,
                                    255, 255, 0, 0, 255}));
  EXPECT_TRUE(DecodeSubtitlePng(gray, &bitmap));
  EXPECT_TRUE(bitmap.rgba ==
              std::vector<uint8_t>({0, 0, 0, 255, 255, 255, 255, 255, 255, 255,
                                    255, 255, 0, 0, 0, 255}));
}

TEST(Mp4Subtitle, ImageProfileRegionsAndPremultipliedPng) {
  std::vector<SubtitleCue> cues;
  std::string requested;
  auto resource = [&](std::string_view uri, std::vector<uint8_t>* bytes) {
    requested = uri;
    *bytes = ImagePng();
    return true;
  };
  EXPECT_TRUE(ParseMp4Subtitle(
      "stpp",
      "<tt ttp:profile='http://www.w3.org/ns/ttml/profile/imsc1/image' "
      "tts:extent='8px 8px'><head><layout><region xml:id='r' "
      "tts:origin='25% 50%' tts:extent='2px 2px'/></layout></head>"
      "<body xml:base='images/'><div region='r' begin='1s' dur='2s' "
      "smpte:backgroundImage='caption.png?x=1&amp;y=2'/></body></tt>",
      1500000, 2000000, &cues, resource));
  EXPECT_EQ(requested, "images/caption.png?x=1&y=2");
  EXPECT_EQ(cues.size(), 1u);
  if (cues.size() != 1 || !cues[0].bitmaps) return;
  EXPECT_EQ(cues[0].start, 1500000);
  EXPECT_EQ(cues[0].end, 3000000);
  EXPECT_EQ(cues[0].canvas_width, 8);
  EXPECT_EQ(cues[0].canvas_height, 8);
  const auto& bitmap = cues[0].bitmaps->at(0);
  EXPECT_EQ(bitmap.x, 2);
  EXPECT_EQ(bitmap.y, 4);
  EXPECT_EQ(bitmap.width, 2);
  EXPECT_EQ(bitmap.height, 2);
  const std::vector<uint8_t> expected = {255, 0, 0,   255, 0, 128, 0, 128,
                                         0,   0, 255, 255, 0, 0,   0, 0};
  EXPECT_TRUE(bitmap.rgba == expected);
  auto decoder = CreateSubtitleDecoder();
  SubtitleConfig config;
  config.codec_name = "ttml";
  config.normalized = true;
  std::string error;
  EXPECT_TRUE(decoder && decoder->Initialize(config, &error));
  if (!decoder) return;
  EncodedPacket packet;
  packet.pts = 5000000;
  packet.duration = 2000000;
  packet.buffer = PackSubtitleCue(cues[0]);
  std::vector<SubtitleCue> decoded;
  EXPECT_TRUE(decoder->Decode(packet, &decoded));
  EXPECT_EQ(decoded.size(), 1u);
  if (decoded.size() == 1 && decoded[0].bitmaps) {
    EXPECT_EQ(decoded[0].start, 5000000);
    EXPECT_EQ(decoded[0].end, 7000000);
    EXPECT_TRUE(decoded[0].bitmaps->at(0).rgba == expected);
  }
}

TEST(Mp4Subtitle, SameStartRegionsSurvivePacketRoundTrip) {
  for (const char* second_end : {"1s", "2s"}) {
    const std::string xml =
        "<tt tts:extent='8px 8px'><head><layout>"
        "<region xml:id='a' tts:origin='1px 1px' tts:extent='2px 2px'/>"
        "<region xml:id='b' tts:origin='5px 1px' tts:extent='2px 2px'/>"
        "</layout></head><body><div region='a' end='2s' "
        "smpte:backgroundImage='a.png'/>"
        "<div region='b' end='" +
        std::string(second_end) +
        "' smpte:backgroundImage='b.png'/></body></tt>";
    std::vector<SubtitleCue> cues;
    EXPECT_TRUE(
        ParseMp4Subtitle("stpp", xml, 0, 3000000, &cues,
                         [](std::string_view, std::vector<uint8_t>* bytes) {
                           *bytes = ImagePng();
                           return true;
                         }));
    EXPECT_EQ(cues.size(), 2u);
    SubtitleRenderer renderer;
    SubtitleConfig config;
    config.codec_name = "ttml";
    config.normalized = true;
    std::string error;
    EXPECT_TRUE(renderer.Initialize(config, &error));
    for (int replay = 0; replay < 2; ++replay) {
      for (const SubtitleCue& cue : cues) {
        EncodedPacket packet;
        packet.pts = cue.start;
        packet.duration = cue.end - cue.start;
        packet.buffer = PackSubtitleCue(cue);
        renderer.Decode(packet);
      }
    }
    auto early = renderer.ImagesAt(500000, 8, 8, 8, 8);
    EXPECT_TRUE(early && early->images.size() == 2);
    if (early && early->images.size() == 2) {
      EXPECT_EQ(early->images[0].x, 1);
      EXPECT_EQ(early->images[1].x, 5);
      EXPECT_EQ(early->images[0].bitmap->rgba[0], 255);
    }
    auto later = renderer.ImagesAt(1500000, 8, 8, 8, 8);
    EXPECT_TRUE(later && later->images.size() ==
                             (std::string_view(second_end) == "1s" ? 1u : 2u));
  }
}

TEST(Mp4Subtitle, Ttml2ImageAndInvalidResources) {
  const std::string prefix =
      "<tt><head><layout><region xml:id='r' tts:origin='1px 2px' "
      "tts:extent='2px 2px'/></layout></head><body region='r' begin='1s'>";
  const std::string body =
      "<div><image begin='500ms' dur='1s' src='urn:mpeg:14496-30:subs:1' "
      "type='image/png' tts:extent='2px 2px'/></div></body></tt>";
  int reads = 0;
  auto resource = [&](std::string_view, std::vector<uint8_t>* bytes) {
    ++reads;
    *bytes = ImagePng();
    return true;
  };
  std::vector<SubtitleCue> cues;
  EXPECT_TRUE(
      ParseMp4Subtitle("stpp", prefix + body, 0, 4000000, &cues, resource));
  EXPECT_EQ(cues.size(), 1u);
  if (cues.size() == 1) {
    EXPECT_EQ(cues[0].start, 1500000);
    EXPECT_EQ(cues[0].end, 2500000);
  }
  reads = 0;
  cues.clear();
  EXPECT_TRUE(!ParseMp4Subtitle("stpp", prefix + body + "<bad", 0, 4000000,
                                &cues, resource));
  EXPECT_EQ(reads, 0);
  EXPECT_TRUE(cues.empty());
  auto huge = [](std::string_view, std::vector<uint8_t>* bytes) {
    *bytes = ImagePng();
    (*bytes)[16] = 0x7f;
    return true;
  };
  EXPECT_TRUE(
      !ParseMp4Subtitle("stpp", prefix + body, 0, 4000000, &cues, huge));
  EXPECT_TRUE(!ParseMp4Subtitle("stpp", prefix + body, 0, 4000000, &cues));
  EXPECT_TRUE(cues.empty());
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
