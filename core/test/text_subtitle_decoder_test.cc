#include "media/text_subtitle_decoder.h"

#include <string>

#include "test.h"

namespace lychee {
namespace {

std::string Plain(const char* codec, std::string_view packet) {
  return TextSubtitleDecoder::ToPlainText(codec, packet);
}

TEST(TextSubtitleDecoder, SubRipMarkup) {
  EXPECT_EQ(Plain("subrip", "Hello <i>world</i>\r\n"), std::string("Hello world"));
  EXPECT_EQ(Plain("subrip", "<font color=\"#ff0000\">red</font> <B>bold</b>"),
            std::string("red bold"));
  EXPECT_EQ(Plain("subrip", "{\\an8}top"), std::string("top"));
  // Angle brackets that are not tags stay.
  EXPECT_EQ(Plain("subrip", "a <3 b <bold>"), std::string("a <3 b <bold>"));
}

TEST(TextSubtitleDecoder, WebVttMarkup) {
  EXPECT_EQ(Plain("webvtt", "<v Bob>Hi</v> <c.loud>there</c>"),
            std::string("Hi there"));
  EXPECT_EQ(Plain("webvtt", "a &lt;b&gt; &amp; c&nbsp;d"), std::string("a <b> & c d"));
  EXPECT_EQ(Plain("webvtt", "one <00:00:01.000>two\n"), std::string("one two"));
}

TEST(TextSubtitleDecoder, AssMarkup) {
  // Matroska's form: ReadOrder, Layer, Style, Name, margins, Effect, Text.
  EXPECT_EQ(Plain("ass", "0,0,Default,,0,0,0,,{\\b1}Bold{\\b0} and, plain\\Nnext"),
            std::string("Bold and, plain\nnext"));
}

TEST(TextSubtitleDecoder, MovText) {
  const std::string packet = std::string("\0\x05Hello", 7) + "styl-box";
  EXPECT_EQ(Plain("mov_text", packet), std::string("Hello"));
  EXPECT_EQ(Plain("mov_text", std::string("\0", 1)), std::string());
  // A length past the packet takes what is there.
  EXPECT_EQ(Plain("mov_text", std::string("\0\x40Hi", 4)), std::string("Hi"));
}

TEST(TextSubtitleDecoder, RejectsBitmapCodecs) {
  TextSubtitleDecoder decoder;
  SubtitleConfig config;
  config.codec_name = "hdmv_pgs_subtitle";
  std::string error;
  EXPECT_TRUE(!decoder.Initialize(config, &error));
  config.codec_name = "subrip";
  EXPECT_TRUE(decoder.Initialize(config, &error));
}

}  // namespace
}  // namespace lychee
