#include <string>

#include "backends/hls/hls_demuxer.h"
#include "backends/hls/hls_playlist.h"
#include "test.h"

namespace lychee {

TEST(HlsPlaylist, ResolvesUrls) {
  const std::string base = "http://a.example/b/c/index.m3u8?token=1";
  EXPECT_EQ(ResolveUrl(base, "seg0.ts"),
            std::string("http://a.example/b/c/seg0.ts"));
  EXPECT_EQ(ResolveUrl(base, "../d/seg.ts?x=2"),
            std::string("http://a.example/b/d/seg.ts?x=2"));
  EXPECT_EQ(ResolveUrl(base, "./e/./f/../g.ts"),
            std::string("http://a.example/b/c/e/g.ts"));
  EXPECT_EQ(ResolveUrl(base, "/root.ts"),
            std::string("http://a.example/root.ts"));
  EXPECT_EQ(ResolveUrl(base, "//cdn.example/x.ts"),
            std::string("http://cdn.example/x.ts"));
  EXPECT_EQ(ResolveUrl(base, "https://o.example/y.ts"),
            std::string("https://o.example/y.ts"));
  EXPECT_EQ(ResolveUrl(base, "../../../../z.ts"),
            std::string("http://a.example/z.ts"));
  EXPECT_EQ(ResolveUrl("http://a.example", "s.ts"),
            std::string("http://a.example/s.ts"));
  EXPECT_EQ(ResolveUrl("/tmp/media/hls/index.m3u8", "../r.ts"),
            std::string("/tmp/media/r.ts"));
  EXPECT_EQ(ResolveUrl("file:///tmp/hls/index.m3u8", "s.ts"),
            std::string("file:///tmp/hls/s.ts"));
}

TEST(HlsPlaylist, ParsesMediaPlaylists) {
  const std::string text =
      "\xEF\xBB\xBF#EXTM3U\r\n"
      "#EXT-X-VERSION:4\n"
      "#EXT-X-TARGETDURATION:6\n"
      "#EXT-X-MEDIA-SEQUENCE:100\n"
      "#EXT-X-DISCONTINUITY-SEQUENCE:7\n"
      "#EXTINF:5.005,title, with comma\n"
      "#EXT-X-BYTERANGE:1000@200\n"
      "media.ts\n"
      "#EXTINF:4.5,\n"
      "#EXT-X-BYTERANGE:500\n"
      "media.ts\n"
      "#EXT-X-DISCONTINUITY\n"
      "#EXT-X-KEY:METHOD=AES-128,URI=\"key.bin\",IV=0x1\n"
      "#EXTINF:6,\n"
      "next.ts\n"
      "#EXT-X-KEY:METHOD=NONE\n"
      "#EXTINF:6.,\n"
      "last.ts\n";
  HlsPlaylist playlist;
  std::string error;
  EXPECT_TRUE(
      ParseHlsPlaylist(text, "http://h/p/list.m3u8", &playlist, &error));
  EXPECT_TRUE(!playlist.master);
  EXPECT_TRUE(!playlist.ended);
  EXPECT_EQ(playlist.target_duration, 6000000);
  EXPECT_EQ(playlist.segments.size(), 4u);
  const HlsSegment& first = playlist.segments[0];
  EXPECT_EQ(first.url, std::string("http://h/p/media.ts"));
  EXPECT_EQ(first.duration, 5005000);
  EXPECT_EQ(first.sequence, 100);
  EXPECT_EQ(first.discontinuity, 7);
  EXPECT_EQ(first.offset, 200);
  EXPECT_EQ(first.length, 1000);
  // A range without an offset follows the previous one.
  EXPECT_EQ(playlist.segments[1].offset, 1200);
  EXPECT_EQ(playlist.segments[1].length, 500);
  EXPECT_EQ(playlist.segments[1].duration, 4500000);
  EXPECT_EQ(playlist.segments[2].discontinuity, 8);
  EXPECT_TRUE(playlist.segments[2].encrypted);
  EXPECT_EQ(playlist.segments[2].offset, -1);
  EXPECT_TRUE(!playlist.segments[3].encrypted);
  EXPECT_EQ(playlist.segments[3].sequence, 103);
  EXPECT_EQ(playlist.segments[3].duration, 6000000);

  EXPECT_TRUE(
      ParseHlsPlaylist("#EXTM3U\n#EXT-X-PLAYLIST-TYPE:VOD\n#EXTINF:1,\na.ts\n",
                       "x", &playlist, &error));
  EXPECT_TRUE(playlist.ended);
  EXPECT_TRUE(!ParseHlsPlaylist("<html>", "x", &playlist, &error));
  EXPECT_TRUE(!ParseHlsPlaylist("", "x", &playlist, &error));
}

TEST(HlsPlaylist, PicksVariants) {
  const std::string text =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"a\",NAME=\"en\",URI=\"audio.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1."
      "64001e,mp4a.40.2\"\n"
      "low.m3u8\n"
      "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=90000,URI=\"iframes.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"hvc1."
      "2.4.L123.B0,mp4a.40.2\"\n"
      "high/index.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=9000000,RESOLUTION=3840x2160,CODECS=\"vp09."
      "00.50.08\"\n"
      "uhd.m3u8\n";
  HlsPlaylist playlist;
  std::string error;
  EXPECT_TRUE(
      ParseHlsPlaylist(text, "http://h/master.m3u8", &playlist, &error));
  EXPECT_TRUE(playlist.master);
  EXPECT_EQ(playlist.variants.size(), 3u);
  EXPECT_EQ(playlist.variants[1].width, 1920);
  EXPECT_EQ(playlist.variants[1].height, 1080);
  EXPECT_EQ(playlist.variants[1].url, std::string("http://h/high/index.m3u8"));
  EXPECT_EQ(PickHlsVariant(playlist.variants), 1);
  // Without CODECS a variant is assumed playable; equal bandwidth goes
  // to the larger picture.
  std::vector<HlsVariant> plain(2);
  plain[0].bandwidth = plain[1].bandwidth = 100;
  plain[0].width = 640;
  plain[0].height = 360;
  plain[1].width = 1280;
  plain[1].height = 720;
  EXPECT_EQ(PickHlsVariant(plain), 1);
  plain[0].codecs = plain[1].codecs = "av01.0.08M.08";
  EXPECT_EQ(PickHlsVariant(plain), -1);
}

TEST(HlsPlaylist, Detects) {
  const char playlist[] = "#EXTM3U\n#EXT-X-VERSION:3\n";
  const char bom[] = "\xEF\xBB\xBF#EXTM3U\n";
  const char other[] = "#EXT-X-VERSION:3\n";
  EXPECT_TRUE(HlsDemuxer::Detect(reinterpret_cast<const uint8_t*>(playlist),
                                 sizeof(playlist) - 1));
  EXPECT_TRUE(HlsDemuxer::Detect(reinterpret_cast<const uint8_t*>(bom),
                                 sizeof(bom) - 1));
  EXPECT_TRUE(!HlsDemuxer::Detect(reinterpret_cast<const uint8_t*>(other),
                                  sizeof(other) - 1));
}

}  // namespace lychee
