// Fragmented MP4 layouts FFmpeg's muxer does not write (mehd, no tfdt),
// and tx3g text (FFmpeg-free builds only).

#include "backends/mp4/mp4_demuxer.h"
#include "backends/hls/hls_demuxer.h"
#include "media/text_subtitle_decoder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
Bytes FragmentedFile(int fragments,
                     uint32_t mehd_ms,
                     bool tfdt,
                     const char* codec = "avc1",
                     const Bytes& codec_config = {}) {
  Bytes entry = Append(Zeros(24), Fields({{64, 2}, {48, 2}}));  // 64x48
  entry = Append(entry, Zeros(50));
  entry = Append(entry, codec_config);
  Bytes stbl =
      Box("stbl", {Box("stsd", {Fields({{0, 4}, {1, 4}}), Box(codec, {entry})}),
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

TEST(Mp4Demuxer, Vp9SampleEntryCarriesHdrColors) {
  // vpcC v1: profile 2, level 3.1, 10-bit full-range BT.2020 PQ.
  const Bytes config = {1, 0, 0, 0, 2, 31, 0xa1, 9, 16, 9, 0, 0};
  std::string error;
  auto demuxer = OpenBytes(
      FragmentedFile(1, 0, true, "vp09", Box("vpcC", {config})), &error);
  EXPECT_TRUE(demuxer != nullptr);
  if (!demuxer) {
    return;
  }
  const auto& video = demuxer->media_info().streams[0].video;
  EXPECT_TRUE(video.codec == VideoCodec::kVP9);
  EXPECT_EQ(video.codec_name, "vp9");
  EXPECT_EQ(video.color.primaries, 9);
  EXPECT_EQ(video.color.transfer, 16);
  EXPECT_EQ(video.color.matrix, 9);
  EXPECT_TRUE(video.color.full_range);
}

TEST(Mp4Demuxer, Av1SampleEntryKeepsConfigurationRecord) {
  const Bytes config = {0x81, 0, 0, 0};
  std::string error;
  auto demuxer = OpenBytes(
      FragmentedFile(1, 0, true, "av01", Box("av1C", {config})), &error);
  EXPECT_TRUE(demuxer != nullptr);
  if (!demuxer) {
    return;
  }
  const auto& video = demuxer->media_info().streams[0].video;
  EXPECT_TRUE(video.codec == VideoCodec::kAV1);
  EXPECT_EQ(video.codec_name, "av1");
  EXPECT_TRUE(video.extradata == config);
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

Bytes SubtitleFile(const char* codec,
                   const Bytes& payload,
                   const Bytes& subs = {}) {
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
         Box("traf",
             {Box("tfhd", {Fields({{0x20000, 4}, {1, 4}})}),
              Box("tfdt", {Fields({{0, 4}, {0, 4}})}),
              Box("trun", {Fields({{1, 4}, {1, 4}, {offset, 4}})}), subs})});
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
  EXPECT_EQ(demuxer->media_info().streams[0].subtitle.codec_name, "ttml");
  auto read = [&] {
    EncodedPacket packet;
    EXPECT_TRUE(demuxer->Read(&packet) == DemuxerReadResult::kOk);
    return packet;
  };
  EncodedPacket first = read();
  EXPECT_EQ(first.pts, 100000);
  EXPECT_EQ(first.duration, 500000);
  SubtitleCue normalized;
  EXPECT_TRUE(UnpackSubtitleCue(first, &normalized));
  EXPECT_EQ(normalized.text, "Hello & <b>");
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

TEST(Mp4Demuxer, ImscAuxiliarySubsamplesAndSeek) {
  const std::string xml =
      "<tt tts:extent='8px 8px'><head><layout><region xml:id='r' "
      "tts:origin='2px 4px' tts:extent='2px 2px'/></layout></head>"
      "<body><div region='r' begin='100ms' end='900ms' "
      "smpte:backgroundImage='urn:mpeg:14496-30:subs:1'/></body></tt>";
  const Bytes png = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00,
                     0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02,
                     0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
                     0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x15, 0x49, 0x44, 0x41,
                     0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x08,
                     0x1b, 0x18, 0x80, 0xf4, 0x7f, 0x10, 0x0b, 0x00, 0x40, 0xd6,
                     0x07, 0x7a, 0x78, 0xd2, 0x6e, 0xe1, 0x00, 0x00, 0x00, 0x00,
                     0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
  for (int version : {0, 1}) {
    const Bytes payload = Append(AsBytes(xml), png);
    const Bytes sizes = Fields({{xml.size(), version ? 4u : 2u},
                                {0, 6},
                                {png.size(), version ? 4u : 2u},
                                {0, 6}});
    const Bytes subs =
        Box("subs", {Fields({{static_cast<uint32_t>(version) << 24, 4},
                             {1, 4},
                             {1, 4},
                             {2, 2}}),
                     sizes});
    std::string error;
    auto demuxer = OpenBytes(SubtitleFile("stpp", payload, subs), &error);
    EXPECT_TRUE(demuxer != nullptr);
    if (!demuxer) continue;
    for (int pass = 0; pass < 2; ++pass) {
      EncodedPacket packet;
      EXPECT_TRUE(demuxer->Read(&packet) == DemuxerReadResult::kOk);
      SubtitleCue cue;
      EXPECT_TRUE(UnpackSubtitleCue(packet, &cue));
      EXPECT_EQ(cue.start, 100000);
      EXPECT_EQ(cue.end, 900000);
      EXPECT_EQ(cue.canvas_width, 8);
      EXPECT_TRUE(cue.bitmaps && cue.bitmaps->size() == 1);
      if (cue.bitmaps && !cue.bitmaps->empty()) {
        EXPECT_EQ(cue.bitmaps->at(0).x, 2);
        EXPECT_EQ(cue.bitmaps->at(0).y, 4);
        EXPECT_EQ(cue.bitmaps->at(0).rgba[0], 255);
      }
      EXPECT_TRUE(demuxer->Read(&packet) == DemuxerReadResult::kEndOfStream);
      EXPECT_TRUE(demuxer->Seek(0));
    }
    Bytes broken = subs;
    broken.back() = 1;  // Reserved/discard fields do not affect the mapping.
    EXPECT_TRUE(OpenBytes(SubtitleFile("stpp", payload, broken), &error) !=
                nullptr);
    // A claimed first sub-sample larger than the sample must be rejected.
    broken = subs;
    broken[22] = 0xff;
    broken[23] = 0xff;
    auto invalid = OpenBytes(SubtitleFile("stpp", payload, broken), &error);
    EncodedPacket packet;
    EXPECT_TRUE(!invalid ||
                invalid->Read(&packet) == DemuxerReadResult::kError);
  }
}

TEST(Mp4Demuxer, InterruptsAnExternalSubtitleResource) {
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool interrupted = false;
  };
  class Blocked : public DataSource {
   public:
    explicit Blocked(std::shared_ptr<State> state) : state_(std::move(state)) {}
    int64_t Read(int64_t, uint8_t*, size_t) override {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->entered = true;
      state_->cv.notify_all();
      state_->cv.wait(lock, [&] { return state_->interrupted; });
      return -1;
    }
    int64_t size() const override { return -1; }
    void Interrupt() override {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->interrupted = true;
      state_->cv.notify_all();
    }
    std::string error() const override { return "interrupted"; }

   private:
    std::shared_ptr<State> state_;
  };
  const std::string xml =
      "<tt><head><layout><region xml:id='r' tts:extent='2px 2px'/>"
      "</layout></head><body><div region='r' smpte:backgroundImage='slow.png'/>"
      "</body></tt>";
  auto state = std::make_shared<State>();
  Mp4Demuxer demuxer;
  std::string error;
  EXPECT_TRUE(demuxer.Open(
      CreateMemoryDataSource(SubtitleFile("stpp", AsBytes(xml))), &error,
      "http://test/subs.mp4", [&](const std::string& url, std::string*) {
        EXPECT_EQ(url, "http://test/slow.png");
        return std::make_unique<Blocked>(state);
      }));
  DemuxerReadResult result = DemuxerReadResult::kOk;
  std::thread reader([&] {
    EncodedPacket packet;
    result = demuxer.Read(&packet);
  });
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    EXPECT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(5),
                                   [&] { return state->entered; }));
  }
  demuxer.Interrupt();
  reader.join();
  EXPECT_TRUE(result == DemuxerReadResult::kAborted);
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

class RateSource : public DataSource {
 public:
  explicit RateSource(Bytes bytes)
      : source_(CreateMemoryDataSource(std::move(bytes))) {}
  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    return source_->Read(offset, data, size);
  }
  int64_t size() const override { return source_->size(); }
  std::string error() const override { return source_->error(); }
  int64_t transfer_rate() const override { return 1000000; }

 private:
  std::unique_ptr<DataSource> source_;
};

TEST(HlsDemuxer, AdaptsFragmentedVariantsAtTheNextSegment) {
  std::map<std::string, Bytes> resources;
  std::map<std::string, int> fetched;
  auto text = [&](const std::string& path, const std::string& value) {
    resources["http://test/" + path] = AsBytes(value);
  };
  text("master.m3u8",
       "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=64x48,CODECS="
       "\"avc1.64001e\"\n"
       "low/"
       "list.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=64x48,CODECS="
       "\"avc1.64001e\"\n"
       "high/list.m3u8\n");
  const std::string playlist =
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MAP:URI=\"init.mp4\"\n"
      "#EXTINF:1,\nseg0.m4s\n#EXTINF:1,\nseg1.m4s\n#EXT-X-ENDLIST\n";
  Bytes first = FragmentedFile(1, 0, true);
  Bytes second = first;
  const uint8_t kTfdt[] = {'t', 'f', 'd', 't'};
  auto at = std::search(second.begin(), second.end(), std::begin(kTfdt),
                        std::end(kTfdt));
  EXPECT_TRUE(at != second.end());
  if (at == second.end()) {
    return;
  }
  // Version/flags then a decode time of 1000 ticks (the next second).
  at[8] = 0;
  at[9] = 0;
  at[10] = 3;
  at[11] = 0xe8;
  // Enough transferred bytes to measure bandwidth rather than latency.
  const Bytes padding = Box("free", {Zeros(64 << 10)});
  first = Append(first, padding);
  second = Append(second, padding);
  for (const std::string level : {"high", "low"}) {
    text(level + "/list.m3u8", playlist);
    resources["http://test/" + level + "/init.mp4"] = {};
    resources["http://test/" + level + "/seg0.m4s"] = first;
    resources["http://test/" + level + "/seg1.m4s"] = second;
  }
  HlsDemuxer demuxer;
  demuxer.set_opener([&](const std::string& url,
                         std::string* error) -> std::unique_ptr<DataSource> {
    auto found = resources.find(url);
    if (found == resources.end()) {
      *error = "not found";
      return nullptr;
    }
    ++fetched[url];
    return std::make_unique<RateSource>(found->second);
  });
  std::string error;
  EXPECT_TRUE(demuxer.Open("http://test/master.m3u8", &error));
  demuxer.SetEnabledStreams({0});
  int packets = 0;
  int64_t last = -1;
  EncodedPacket packet;
  while (packets < 30 && demuxer.Read(&packet) == DemuxerReadResult::kOk) {
    EXPECT_EQ(packet.stream_index, 0);
    last = packet.pts;
    ++packets;
  }
  EXPECT_EQ(packets, 20);
  EXPECT_EQ(last, 1900000);
  EXPECT_EQ(fetched["http://test/high/seg1.m4s"], 0);
  EXPECT_EQ(fetched["http://test/low/seg1.m4s"], 1);
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
