#include <cstdint>
#include <string>
#include <vector>

#include "backends/dash/dash_demuxer.h"
#include "backends/dash/mpd.h"
#include "backends/dash/xml.h"
#include "test.h"

// The harness has only EXPECT_*; these stop the test where going on
// would index past what was checked.
#define ASSERT_TRUE(cond)   \
  do {                      \
    const bool ok = (cond); \
    EXPECT_TRUE(ok);        \
    if (!ok) return;        \
  } while (0)
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

namespace lychee {

// NOLINTBEGIN(bugprone-throwing-static-initialization)

TEST(Xml, ParsesElementsAttributesAndText) {
  XmlElement root;
  std::string error;
  ASSERT_TRUE(ParseXml(
      "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<!DOCTYPE x [ <!ENTITY a \"b\"> "
      "]>\n"
      "<!-- c --><a:Root xmlns:a=\"urn:x\" one='1' "
      "two=\"&lt;&amp;&#x41;&#66;&bogus;\">"
      "text <![CDATA[<raw>]]><Child k=\"v\"/><!-- d --><Child>t&gt;</Child>"
      "<?pi x?></a:Root>\n",
      &root, &error));
  EXPECT_EQ(root.name, std::string("Root"));
  EXPECT_EQ(*root.Attribute("one"), std::string("1"));
  EXPECT_EQ(*root.Attribute("two"), std::string("<&AB&bogus;"));
  EXPECT_TRUE(root.Attribute("three") == nullptr);
  EXPECT_EQ(root.text, std::string("text <raw>"));
  ASSERT_EQ(root.Children("Child").size(), 2u);
  EXPECT_EQ(*root.Child("Child")->Attribute("k"), std::string("v"));
  EXPECT_EQ(root.Children("Child")[1]->text, std::string("t>"));
}

TEST(Xml, RefusesMalformedDocuments) {
  XmlElement root;
  std::string error;
  for (const char* bad : {"", "text", "<a>", "<a></b>", "<a b=c/>", "<a b/>",
                          "<a/><b/>", "<a><!-- </a>", "<a b=\"1></a>"}) {
    EXPECT_FALSE(ParseXml(bad, &root, &error));
  }
  std::string deep;
  for (int i = 0; i < 100; ++i)
    deep += "<a>";
  EXPECT_FALSE(ParseXml(deep, &root, &error));
  EXPECT_TRUE(error.find("deep") != std::string::npos);
}

TEST(Dash, ParsesIsoDurations) {
  int64_t micros = 0;
  ASSERT_TRUE(ParseIsoDuration("PT1H2M3.5S", &micros));
  EXPECT_EQ(micros, int64_t{3723500000});
  ASSERT_TRUE(ParseIsoDuration("P1DT0.25S", &micros));
  EXPECT_EQ(micros, int64_t{86400250000});
  EXPECT_FALSE(ParseIsoDuration("1H", &micros));
  EXPECT_FALSE(ParseIsoDuration("PT", &micros));
  EXPECT_FALSE(ParseIsoDuration("PT5X", &micros));
}

TEST(Dash, ExpandsTemplates) {
  DashRepresentation rep;
  rep.id = "v1";
  rep.bandwidth = 500000;
  EXPECT_EQ(ExpandDashTemplate("$RepresentationID$/$Number%05d$-$Time$-"
                               "$Bandwidth$$$.m4s?$Unknown$",
                               rep, 42, 90000),
            std::string("v1/00042-90000-500000$.m4s?$Unknown$"));
  EXPECT_EQ(ExpandDashTemplate("a$Number$", rep, 7, 0), std::string("a7"));
  EXPECT_EQ(ExpandDashTemplate("a$Number", rep, 7, 0), std::string("a$Number"));
}

TEST(Dash, DetectsManifests) {
  const std::string mpd = "\xEF\xBB\xBF  <?xml version=\"1.0\"?><MPD>";
  EXPECT_TRUE(DashDemuxer::Detect(reinterpret_cast<const uint8_t*>(mpd.data()),
                                  mpd.size()));
  const std::string m3u8 = "#EXTM3U\n<MPD";
  EXPECT_FALSE(DashDemuxer::Detect(
      reinterpret_cast<const uint8_t*>(m3u8.data()), m3u8.size()));
}

namespace {

constexpr char kManifest[] = R"(<?xml version="1.0" encoding="utf-8"?>
<MPD xmlns="urn:mpeg:dash:schema:mpd:2011" type="static"
     mediaPresentationDuration="PT10S">
  <BaseURL>http://cdn.example/a/</BaseURL>
  <Period>
    <AdaptationSet contentType="video" frameRate="30000/1001">
      <SegmentTemplate timescale="90000" initialization="$RepresentationID$/i.mp4"
                       media="$RepresentationID$/$Number%03d$-$Time$.m4s" startNumber="5">
        <SegmentTimeline><S t="900" d="180000" r="1"/><S d="90000" r="-1"/></SegmentTimeline>
      </SegmentTemplate>
      <Representation id="hi" mimeType="video/mp4" codecs="avc1.64001f"
                      bandwidth="2000000" width="1280" height="720"/>
      <Representation id="lo" mimeType="video/mp4" codecs="avc1.64001e"
                      bandwidth="400000" width="640" height="360">
        <BaseURL>low/</BaseURL>
        <SegmentTemplate duration="3" timescale="1" media="s$Number$.m4s"
                         initialization="init.mp4"/>
      </Representation>
    </AdaptationSet>
    <AdaptationSet mimeType="audio/mp4" lang="fr">
      <Role schemeIdUri="urn:mpeg:dash:role:2011" value="main"/>
      <Accessibility schemeIdUri="urn:tva:metadata:cs:AudioPurposeCS:2007" value="1"/>
      <Label>Fran&#xE7;ais</Label>
      <Representation id="a" codecs="mp4a.40.2" bandwidth="96000">
        <BaseURL>audio.mp4</BaseURL>
        <SegmentBase indexRange="700-799"><Initialization range="0-699"/></SegmentBase>
      </Representation>
      <Representation id="b" codecs="mp4a.40.2" bandwidth="64000">
        <BaseURL>b.mp4</BaseURL>
        <SegmentList timescale="1000" duration="4000">
          <Initialization range="0-99"/>
          <SegmentURL mediaRange="100-199"/><SegmentURL media="b2.m4s"/>
        </SegmentList>
      </Representation>
    </AdaptationSet>
    <AdaptationSet contentType="text" mimeType="text/vtt" lang="en">
      <Role schemeIdUri="urn:mpeg:dash:role:2011" value="forced-subtitle"/>
      <Representation id="s" bandwidth="1"><BaseURL>../subs.vtt</BaseURL></Representation>
    </AdaptationSet>
  </Period>
</MPD>
)";

}  // namespace

TEST(Dash, ParsesManifests) {
  DashManifest manifest;
  std::string error;
  ASSERT_TRUE(
      ParseMpd(kManifest, "http://host/m/manifest.mpd?t=1", &manifest, &error));
  EXPECT_FALSE(manifest.dynamic);
  EXPECT_EQ(manifest.duration, int64_t{10000000});
  ASSERT_EQ(manifest.adaptation_sets.size(), 3u);
  const DashAdaptationSet& video = manifest.adaptation_sets[0];
  EXPECT_TRUE(video.type == DashAdaptationSet::Type::kVideo);
  ASSERT_EQ(video.representations.size(), 2u);
  EXPECT_EQ(video.representations[0].frame_rate > 29.97 &&
                video.representations[0].frame_rate < 29.98,
            true);
  EXPECT_EQ(video.representations[1].base_url,
            std::string("http://cdn.example/a/low/"));
  const DashAdaptationSet& audio = manifest.adaptation_sets[1];
  EXPECT_TRUE(audio.type == DashAdaptationSet::Type::kAudio);
  EXPECT_EQ(audio.language, std::string("fr"));
  EXPECT_EQ(audio.label, std::string("Fran\xC3\xA7"
                                     "ais"));
  EXPECT_TRUE(audio.main);
  EXPECT_TRUE(audio.role == TrackRole::kDescription);
  const DashAdaptationSet& text = manifest.adaptation_sets[2];
  EXPECT_TRUE(text.type == DashAdaptationSet::Type::kText);
  EXPECT_TRUE(text.forced);

  // A SegmentTimeline: t from the first S, r="-1" up to the end.
  HlsPlaylist playlist;
  ASSERT_TRUE(DashMediaPlaylist(video.representations[0], video.type,
                                manifest.duration, {}, &playlist, &error));
  EXPECT_TRUE(playlist.ended);
  ASSERT_EQ(playlist.segments.size(), 8u);
  EXPECT_EQ(playlist.segments[0].url,
            std::string("http://cdn.example/a/hi/005-900.m4s"));
  EXPECT_EQ(playlist.segments[1].url,
            std::string("http://cdn.example/a/hi/006-180900.m4s"));
  EXPECT_EQ(playlist.segments[2].duration, int64_t{1000000});
  EXPECT_EQ(playlist.segments[7].sequence, int64_t{12});
  EXPECT_TRUE(playlist.segments[0].initialization);
  EXPECT_EQ(playlist.segments[0].map.url,
            std::string("http://cdn.example/a/hi/i.mp4"));
  // @duration, the start number inherited: the last segment ends with the
  // Period.
  ASSERT_TRUE(DashMediaPlaylist(video.representations[1], video.type,
                                manifest.duration, {}, &playlist, &error));
  ASSERT_EQ(playlist.segments.size(), 4u);
  EXPECT_EQ(playlist.segments[3].url,
            std::string("http://cdn.example/a/low/s8.m4s"));
  EXPECT_EQ(playlist.segments[3].duration, int64_t{1000000});
  // SegmentBase: needs its index.
  EXPECT_FALSE(DashMediaPlaylist(audio.representations[0], audio.type,
                                 manifest.duration, {}, &playlist, &error));
  ASSERT_TRUE(DashMediaPlaylist(
      audio.representations[0], audio.type, manifest.duration,
      {{800, 1000, 4000000}, {1800, 500, 6000000}}, &playlist, &error));
  ASSERT_EQ(playlist.segments.size(), 2u);
  EXPECT_EQ(playlist.segments[1].offset, int64_t{1800});
  EXPECT_EQ(playlist.segments[1].length, int64_t{500});
  EXPECT_EQ(playlist.segments[0].map.offset, int64_t{0});
  EXPECT_EQ(playlist.segments[0].map.length, int64_t{700});
  // SegmentList: byte ranges of the BaseURL and URLs of their own.
  ASSERT_TRUE(DashMediaPlaylist(audio.representations[1], audio.type,
                                manifest.duration, {}, &playlist, &error));
  ASSERT_EQ(playlist.segments.size(), 2u);
  EXPECT_EQ(playlist.segments[0].url,
            std::string("http://cdn.example/a/b.mp4"));
  EXPECT_EQ(playlist.segments[0].offset, int64_t{100});
  EXPECT_EQ(playlist.segments[1].url,
            std::string("http://cdn.example/a/b2.m4s"));
  EXPECT_EQ(playlist.segments[1].duration, int64_t{4000000});
  EXPECT_EQ(playlist.segments[0].map.length, int64_t{100});
  // A WebVTT sidecar: one segment.
  ASSERT_TRUE(DashMediaPlaylist(text.representations[0], text.type,
                                manifest.duration, {}, &playlist, &error));
  ASSERT_EQ(playlist.segments.size(), 1u);
  EXPECT_EQ(playlist.segments[0].url,
            std::string("http://cdn.example/subs.vtt"));
  EXPECT_FALSE(playlist.segments[0].initialization);
}

TEST(Dash, ParsesSegmentIndexes) {
  // A version 0 'sidx' of two references, the first 100 bytes after it.
  std::vector<uint8_t> box = {
      0, 0, 0,    56,   's', 'i', 'd',  'x',  //
      0, 0, 0,    0,                          // Version 0, flags.
      0, 0, 0,    1,                          // reference_ID.
      0, 0, 0x03, 0xe8,                       // timescale 1000.
      0, 0, 0,    0,                          // earliest_presentation_time.
      0, 0, 0,    100,                        // first_offset.
      0, 0, 0,    2,                          // reference_count.
      0, 0, 0x10, 0,    0,   0,   0x07, 0xd0, 0x90, 0, 0, 0,  // 4096 B, 2 s.
      0, 0, 0x20, 0,    0,   0,   0x0b, 0xb8, 0x90, 0, 0, 0,  // 8192 B, 3 s.
  };
  std::vector<DashSubsegment> index;
  ASSERT_TRUE(ParseSidx(box.data(), box.size(), 1000, &index));
  ASSERT_EQ(index.size(), 2u);
  EXPECT_EQ(index[0].offset, int64_t{1000 + 56 + 100});
  EXPECT_EQ(index[0].length, int64_t{4096});
  EXPECT_EQ(index[0].duration, int64_t{2000000});
  EXPECT_EQ(index[1].offset, int64_t{1156 + 4096});
  EXPECT_EQ(index[1].length, int64_t{8192});
  EXPECT_EQ(index[1].duration, int64_t{3000000});
  // Truncated, or a hierarchical index.
  EXPECT_FALSE(ParseSidx(box.data(), box.size() - 1, 0, &index));
  box[32] |= 0x80;
  EXPECT_FALSE(ParseSidx(box.data(), box.size(), 0, &index));
}

TEST(Dash, RefusesWhatItCannotPlay) {
  DashManifest manifest;
  std::string error;
  EXPECT_FALSE(ParseMpd("<html/>", "x", &manifest, &error));
  EXPECT_FALSE(ParseMpd("<MPD/>", "x", &manifest, &error));
  ASSERT_TRUE(
      ParseMpd("<MPD type='dynamic'><Period><AdaptationSet contentType='video'>"
               "<Representation id='1'/></AdaptationSet></Period></MPD>",
               "x", &manifest, &error));
  EXPECT_TRUE(manifest.dynamic);
  HlsPlaylist playlist;
  // No segment information for fragmented MP4.
  EXPECT_FALSE(DashMediaPlaylist(manifest.adaptation_sets[0].representations[0],
                                 DashAdaptationSet::Type::kVideo, kNoTimestamp,
                                 {}, &playlist, &error));
  // A timeline that would list billions of segments.
  ASSERT_TRUE(ParseMpd(
      "<MPD><Period><AdaptationSet contentType='video'><Representation id='1'>"
      "<SegmentTemplate media='$Number$' initialization='i'>"
      "<SegmentTimeline><S d='1' r='9000000000'/></SegmentTimeline>"
      "</SegmentTemplate></Representation></AdaptationSet></Period></MPD>",
      "x", &manifest, &error));
  EXPECT_FALSE(DashMediaPlaylist(manifest.adaptation_sets[0].representations[0],
                                 DashAdaptationSet::Type::kVideo, kNoTimestamp,
                                 {}, &playlist, &error));
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace lychee
