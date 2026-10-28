// Fragmented MP4 layouts FFmpeg's muxer does not write (mehd, no tfdt),
// and tx3g text (FFmpeg-free builds only).

#include "backends/mp4/mp4_demuxer.h"
#include "backends/hls/hls_demuxer.h"
#include "media/text_subtitle_decoder.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "test.h"

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)

namespace lychee {
namespace {

using Bytes = std::vector<uint8_t>;

Bytes AsBytes(std::string_view text) {
  Bytes bytes(text.size());
  if (!text.empty()) {
    std::memcpy(bytes.data(), text.data(), text.size());
  }
  return bytes;
}

// Fields wider than eight bytes (zeros, e.g. mdhd's times) start with
// zeros.
void Put(Bytes* out, uint64_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) {
    out->push_back(i < 8 ? static_cast<uint8_t>(value >> (8 * i)) : 0);
  }
}

Bytes Box(const char* type, std::initializer_list<Bytes> parts) {
  Bytes payload;
  for (const Bytes& part : parts) {
    payload.insert(payload.end(), part.begin(), part.end());
  }
  Bytes box;
  Put(&box, payload.size() + 8, 4);
  const auto* code = reinterpret_cast<const uint8_t*>(type);
  box.insert(box.end(), code, code + 4);
  box.insert(box.end(), payload.begin(), payload.end());
  return box;
}

// Big-endian fields of the given byte widths.
Bytes Fields(std::initializer_list<std::pair<uint64_t, int>> fields) {
  Bytes out;
  for (const auto& [value, bytes] : fields) {
    Put(&out, value, bytes);
  }
  return out;
}

Bytes Zeros(size_t count) {
  return Bytes(count, 0);
}

Bytes Append(Bytes a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

constexpr uint32_t kTimescale = 1000;
constexpr uint32_t kSampleMs = 100;
constexpr uint32_t kSampleSize = 10;
constexpr int kSamplesPerFragment = 10;

// A video track of 100 ms samples of 10 bytes in fragments of one second,
// each starting with a keyframe; with mehd if |mehd_ms| > 0, and tfdt
// boxes if |tfdt|.
Bytes FragmentedFile(int fragments, uint32_t mehd_ms, bool tfdt) {
  Bytes entry = Append(Zeros(24), Fields({{64, 2}, {48, 2}}));  // 64x48
  entry = Append(entry, Zeros(50));
  Bytes stbl = Box(
      "stbl", {Box("stsd", {Fields({{0, 4}, {1, 4}}), Box("avc1", {entry})}),
               Box("stts", {Zeros(8)}), Box("stsc", {Zeros(8)}),
               Box("stsz", {Zeros(12)}), Box("stco", {Zeros(8)})});
  Bytes trak =
      Box("trak",
          {Box("tkhd", {Fields({{3, 4}, {0, 8}, {1, 4}}), Zeros(68)}),
           Box("mdia",
               {Box("mdhd", {Fields({{0, 12}, {kTimescale, 4}, {0, 8}})}),
                Box("hdlr", {Zeros(8), Bytes{'v', 'i', 'd', 'e'}, Zeros(12)}),
                Box("minf", {stbl})})});
  Bytes mvex = Box("mvex", {Box("trex", {Fields({{0, 4},
                                                 {1, 4},
                                                 {1, 4},
                                                 {kSampleMs, 4},
                                                 {kSampleSize, 4},
                                                 {0x10000, 4}})})});
  if (mehd_ms > 0) {
    mvex = Box("mvex", {Box("mehd", {Fields({{0, 4}, {mehd_ms, 4}})}),
                        Bytes(mvex.begin() + 8, mvex.end())});
  }
  Bytes file = Append(
      Box("ftyp", {Bytes{'i', 's', 'o', 'm'}, Zeros(4)}),
      Box("moov", {Box("mvhd", {Fields({{0, 12}, {kTimescale, 4}}), Zeros(84)}),
                   trak, mvex}));
  for (int i = 0; i < fragments; ++i) {
    Bytes tfhd = Box("tfhd", {Fields({{0x20000, 4}, {1, 4}})});
    Bytes decode =
        tfdt ? Box("tfdt", {Fields({{0, 4}, {uint64_t{1000} * i, 4}})})
             : Bytes();
    // trun: data offset and first sample flags (a keyframe); the rest
    // from trex. The data starts after the moof and the mdat header.
    auto moof_with = [&](uint32_t data_offset) {
      Bytes trun = Box(
          "trun",
          {Fields(
              {{0x5, 4}, {kSamplesPerFragment, 4}, {data_offset, 4}, {0, 4}})});
      return Box(
          "moof",
          {Box("mfhd", {Fields({{0, 4}, {static_cast<uint64_t>(i) + 1, 4}})}),
           Box("traf", {tfhd, decode, trun})});
    };
    const size_t moof_size = moof_with(0).size();
    file = Append(file, moof_with(static_cast<uint32_t>(moof_size + 8)));
    file =
        Append(file, Box("mdat", {Zeros(kSamplesPerFragment * kSampleSize)}));
  }
  return file;
}

std::unique_ptr<Mp4Demuxer> OpenBytes(Bytes bytes, std::string* error) {
  auto demuxer = std::make_unique<Mp4Demuxer>();
  if (!demuxer->Open(CreateMemoryDataSource(std::move(bytes)), error)) {
    return nullptr;
  }
  return demuxer;
}

// The pts of every packet to the end, in milliseconds.
std::vector<int64_t> ReadAll(Mp4Demuxer* demuxer, DemuxerReadResult* last) {
  std::vector<int64_t> times;
  EncodedPacket packet;
  while ((*last = demuxer->Read(&packet)) == DemuxerReadResult::kOk) {
    times.push_back(packet.pts / 1000);
  }
  return times;
}

TEST(Mp4Demuxer, FragmentDurationFromMehd) {
  std::string error;
  auto demuxer = OpenBytes(FragmentedFile(5, 4800, true), &error);
  EXPECT_TRUE(demuxer != nullptr);
  if (!demuxer) {
    return;
  }
  // mehd, not the fragments (5 s).
  EXPECT_EQ(demuxer->media_info().duration, 4800000);
  // Seek points come from reading the moofs only now.
  EXPECT_TRUE(demuxer->Seek(3500000));
  DemuxerReadResult last;
  std::vector<int64_t> times = ReadAll(demuxer.get(), &last);
  EXPECT_TRUE(last == DemuxerReadResult::kEndOfStream);
  EXPECT_EQ(times.size(), 20u);
  EXPECT_EQ(times.empty() ? -1 : times.front(), 3000);
  EXPECT_EQ(times.empty() ? -1 : times.back(), 4900);
  EXPECT_TRUE(demuxer->Seek(200000));
  times = ReadAll(demuxer.get(), &last);
  EXPECT_EQ(times.size(), 50u);
  EXPECT_EQ(times.empty() ? -1 : times.front(), 0);
}

TEST(Mp4Demuxer, FragmentsWithoutTfdt) {
  std::string error;
  auto demuxer = OpenBytes(FragmentedFile(5, 0, false), &error);
  EXPECT_TRUE(demuxer != nullptr);
  if (!demuxer) {
    return;
  }
  // Read to the end at open, each fragment after the last.
  EXPECT_EQ(demuxer->media_info().duration, 5000000);
  DemuxerReadResult last;
  std::vector<int64_t> times = ReadAll(demuxer.get(), &last);
  EXPECT_EQ(times.size(), 50u);
  for (size_t i = 0; i < times.size(); ++i) {
    EXPECT_EQ(times[i], static_cast<int64_t>(i) * kSampleMs);
  }
  EXPECT_TRUE(demuxer->Seek(2500000));
  times = ReadAll(demuxer.get(), &last);
  EXPECT_EQ(times.empty() ? -1 : times.front(), 2000);
  EXPECT_EQ(times.size(), 30u);
}

// A trun of four billion samples taking no bytes of its table.
TEST(Mp4Demuxer, HugeDefaultSampleCount) {
  Bytes file = FragmentedFile(1, 0, true);
  const uint8_t kTrun[] = {'t', 'r', 'u', 'n'};
  auto at =
      std::search(file.begin(), file.end(), std::begin(kTrun), std::end(kTrun));
  EXPECT_TRUE(at != file.end());
  // version and flags, then sample_count.
  for (int i = 0; i < 4; ++i) {
    at[8 + i] = 0xff;
  }
  std::string error;
  auto demuxer = OpenBytes(file, &error);
  EXPECT_TRUE(demuxer != nullptr);
  if (!demuxer) {
    return;
  }
  // Bounded by the bytes they claim: the file holds fewer than 100.
  DemuxerReadResult last;
  std::vector<int64_t> times = ReadAll(demuxer.get(), &last);
  EXPECT_TRUE(times.size() < 100u);
}

Bytes SubtitleFile(const char* codec, const Bytes& payload) {
  Bytes stbl = Box(
      "stbl", {Box("stsd", {Fields({{0, 4}, {1, 4}}), Box(codec, {Zeros(8)})}),
               Box("stts", {Zeros(8)}), Box("stsc", {Zeros(8)}),
               Box("stsz", {Zeros(12)}), Box("stco", {Zeros(8)})});
  Bytes trak =
      Box("trak",
          {Box("tkhd", {Fields({{3, 4}, {0, 8}, {1, 4}}), Zeros(68)}),
           Box("mdia",
               {Box("mdhd", {Fields({{0, 12}, {kTimescale, 4}, {0, 8}})}),
                Box("hdlr", {Zeros(8), Bytes{'s', 'u', 'b', 't'}, Zeros(12)}),
                Box("minf", {stbl})})});
  Bytes file =
      Box("moov",
          {Box("mvhd", {Fields({{0, 12}, {kTimescale, 4}}), Zeros(84)}), trak,
           Box("mvex", {Box("trex", {Fields({{0, 4},
                                             {1, 4},
                                             {1, 4},
                                             {1000, 4},
                                             {payload.size(), 4},
                                             {0, 4}})})})});
  auto moof = [&](uint32_t offset) {
    return Box(
        "moof",
        {Box("mfhd", {Fields({{0, 4}, {1, 4}})}),
         Box("traf", {Box("tfhd", {Fields({{0x20000, 4}, {1, 4}})}),
                      Box("tfdt", {Fields({{0, 4}, {0, 4}})}),
                      Box("trun", {Fields({{1, 4}, {1, 4}, {offset, 4}})})})});
  };
  file = Append(file, moof(static_cast<uint32_t>(moof(0).size() + 8)));
  return Append(file, Box("mdat", {payload}));
}

TEST(Mp4Demuxer, FragmentedSubtitleSamplesAndSeek) {
  const std::string xml =
      "<tt><body><p begin='100ms' end='600ms'>Hello &amp; &lt;b&gt;</p>"
      "<p begin='700ms' end='900ms'>Later</p></body></tt>";
  std::string error;
  auto demuxer = OpenBytes(SubtitleFile("stpp", AsBytes(xml)), &error);
  EXPECT_TRUE(demuxer != nullptr);
  if (!demuxer) {
    return;
  }
  EXPECT_EQ(demuxer->media_info().streams[0].subtitle.codec_name, "webvtt");
  auto read = [&] {
    EncodedPacket packet;
    EXPECT_TRUE(demuxer->Read(&packet) == DemuxerReadResult::kOk);
    return packet;
  };
  EncodedPacket first = read();
  EXPECT_EQ(first.pts, 100000);
  EXPECT_EQ(first.duration, 500000);
  EXPECT_EQ(
      TextSubtitleDecoder::ToPlainText(
          "webvtt",
          std::string_view(reinterpret_cast<const char*>(first.buffer->data()),
                           first.buffer->size())),
      "Hello & <b>");
  EXPECT_TRUE(demuxer->Seek(0));  // Drops the queued second paragraph.
  EXPECT_EQ(read().pts, 100000);
  demuxer->SetEnabledStreams({0});  // Keeps this track's queued paragraph.
  EXPECT_EQ(read().pts, 700000);
  EXPECT_TRUE(demuxer->Seek(0));
  EXPECT_EQ(read().pts, 100000);
  demuxer->SetEnabledStreams({});
  EncodedPacket packet;
  EXPECT_TRUE(demuxer->Read(&packet) == DemuxerReadResult::kEndOfStream);

  auto webvtt = OpenBytes(
      SubtitleFile("wvtt", Box("vttc", {Box("payl", {Bytes{'H', 'i'}})})),
      &error);
  EXPECT_TRUE(webvtt != nullptr);
  if (webvtt) {
    EXPECT_TRUE(webvtt->Read(&packet) == DemuxerReadResult::kOk);
    EXPECT_EQ(packet.pts, 0);
    EXPECT_EQ(packet.duration, 1000000);
  }
}

TEST(HlsDemuxer, FragmentedSubtitleRenditions) {
  const std::string master =
      "#EXTM3U\n#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"s\",NAME=\"English\","
      "LANGUAGE=\"en\",DEFAULT=YES,URI=\"subs.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1000,CODECS=\"avc1.64001e\",SUBTITLES="
      "\"s\"\n"
      "video.m3u8\n";
  const std::string playlist =
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MAP:URI=\"init.mp4\"\n"
      "#EXTINF:1,\nseg.m4s\n#EXT-X-ENDLIST\n";
  const std::string xml =
      "<tt><body><p begin='100ms' end='600ms'>Hello &amp; IMSC</p></body></tt>";
  for (const char* codec : {"stpp", "wvtt"}) {
    std::map<std::string, Bytes> resources;
    auto text = [&](const std::string& key, const std::string& value) {
      resources["http://test/" + key] = AsBytes(value);
    };
    text("master.m3u8", master);
    text("video.m3u8", playlist);
    text("subs.m3u8", playlist);
    const Bytes video = FragmentedFile(1, 0, true);
    resources["http://test/init.mp4"] = {};
    resources["http://test/seg.m4s"] = video;
    // Distinct URLs for the subtitle rendition, sharing the same source
    // opener as every platform's network/file sources do.
    std::string subs = playlist;
    subs.replace(subs.find("init.mp4"), 8, "sub_init.mp4");
    subs.replace(subs.find("seg.m4s"), 7, "sub_seg.m4s");
    text("subs.m3u8", subs);
    resources["http://test/sub_init.mp4"] = {};
    resources["http://test/sub_seg.m4s"] = SubtitleFile(
        codec, std::string(codec) == "stpp"
                   ? AsBytes(xml)
                   : Box("vttc", {Box("payl", {Bytes{'H', 'i'}})}));
    HlsDemuxer demuxer;
    demuxer.set_opener([&](const std::string& url, std::string* error) {
      auto found = resources.find(url);
      if (found == resources.end()) {
        *error = "not found";
        return std::unique_ptr<DataSource>();
      }
      return CreateMemoryDataSource(found->second);
    });
    std::string error;
    EXPECT_TRUE(demuxer.Open("http://test/master.m3u8", &error));
    const MediaInfo& info = demuxer.media_info();
    EXPECT_EQ(info.streams.size(), 2u);
    EXPECT_EQ(info.default_subtitle_stream, 1);
    EncodedPacket packet;
    int subtitle_packets = 0;
    for (int i = 0; i < 20 && demuxer.Read(&packet) == DemuxerReadResult::kOk;
         ++i) {
      if (packet.stream_index == 1) {
        ++subtitle_packets;
        EXPECT_EQ(packet.pts, std::string(codec) == "stpp" ? 100000 : 0);
      }
    }
    EXPECT_EQ(subtitle_packets, 1);
    EXPECT_TRUE(demuxer.Seek(0));
    subtitle_packets = 0;
    for (int i = 0; i < 20 && demuxer.Read(&packet) == DemuxerReadResult::kOk;
         ++i) {
      subtitle_packets += packet.stream_index == 1;
    }
    EXPECT_EQ(subtitle_packets, 1);
  }
}

TEST(TextSubtitleDecoder, MovText) {
  auto text = [](const Bytes& bytes) {
    return TextSubtitleDecoder::MovTextToPlainText(bytes.data(), bytes.size());
  };
  // Text, then a style box.
  EXPECT_EQ(text(Append(Bytes{0, 5, 'H', 'e', 'l', 'l', 'o'},
                        Box("styl", {Fields({{1, 2}, {0, 4}})}))),
            "Hello");
  EXPECT_EQ(text(Bytes{0, 0}), "");
  EXPECT_EQ(text(Bytes{0}), "");
  // A length past the end; QuickTime's \r breaks; trailing breaks.
  EXPECT_EQ(text(Bytes{0, 99, 'a', '\r', 'b', '\r', '\n', 'c', '\n'}),
            "a\nb\nc");
  // UTF-16 with its byte order mark: "é😀".
  EXPECT_EQ(text(Bytes{0, 8, 0xfe, 0xff, 0x00, 0xe9, 0xd8, 0x3d, 0xde, 0x00}),
            "\xc3\xa9\xf0\x9f\x98\x80");
}

}  // namespace
}  // namespace lychee

// NOLINTEND(bugprone-throwing-static-initialization)
