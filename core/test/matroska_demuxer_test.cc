#include "backends/matroska/matroska_demuxer.h"

#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

using Bytes = std::vector<uint8_t>;

// Element IDs (RFC 9559).
constexpr uint32_t kEbml = 0x1A45DFA3;
constexpr uint32_t kDocType = 0x4282;
constexpr uint32_t kSegment = 0x18538067;
constexpr uint32_t kSeekHead = 0x114D9B74;
constexpr uint32_t kSeek = 0x4DBB;
constexpr uint32_t kSeekId = 0x53AB;
constexpr uint32_t kSeekPosition = 0x53AC;
constexpr uint32_t kInfo = 0x1549A966;
constexpr uint32_t kTimestampScale = 0x2AD7B1;
constexpr uint32_t kDuration = 0x4489;
constexpr uint32_t kTracks = 0x1654AE6B;
constexpr uint32_t kTrackEntry = 0xAE;
constexpr uint32_t kTrackNumber = 0xD7;
constexpr uint32_t kTrackType = 0x83;
constexpr uint32_t kDefaultDuration = 0x23E383;
constexpr uint32_t kCodecId = 0x86;
constexpr uint32_t kCodecDelay = 0x56AA;
constexpr uint32_t kVideo = 0xE0;
constexpr uint32_t kPixelWidth = 0xB0;
constexpr uint32_t kPixelHeight = 0xBA;
constexpr uint32_t kDisplayWidth = 0x54B0;
constexpr uint32_t kDisplayHeight = 0x54BA;
constexpr uint32_t kColour = 0x55B0;
constexpr uint32_t kMatrixCoefficients = 0x55B1;
constexpr uint32_t kRange = 0x55B9;
constexpr uint32_t kTransferCharacteristics = 0x55BA;
constexpr uint32_t kPrimaries = 0x55BB;
constexpr uint32_t kAudio = 0xE1;
constexpr uint32_t kSamplingFrequency = 0xB5;
constexpr uint32_t kChannels = 0x9F;
constexpr uint32_t kContentEncodings = 0x6D80;
constexpr uint32_t kContentEncoding = 0x6240;
constexpr uint32_t kContentCompression = 0x5034;
constexpr uint32_t kContentCompAlgo = 0x4254;
constexpr uint32_t kContentCompSettings = 0x4255;
constexpr uint32_t kCluster = 0x1F43B675;
constexpr uint32_t kTimestamp = 0xE7;
constexpr uint32_t kSimpleBlock = 0xA3;
constexpr uint32_t kBlockGroup = 0xA0;
constexpr uint32_t kBlock = 0xA1;
constexpr uint32_t kBlockDuration = 0x9B;
constexpr uint32_t kReferenceBlock = 0xFB;
constexpr uint32_t kCues = 0x1C53BB6B;
constexpr uint32_t kCuePoint = 0xBB;
constexpr uint32_t kCueTime = 0xB3;
constexpr uint32_t kCueTrackPositions = 0xB7;
constexpr uint32_t kCueTrack = 0xF7;
constexpr uint32_t kCueClusterPosition = 0xF1;

// Sizes are written in eight bytes, so an element's length does not
// depend on the values in it.
const Bytes kUnknownSize = {0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

Bytes Cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

Bytes Id(uint32_t id) {
  Bytes out;
  for (int shift = 24; shift >= 0; shift -= 8) {
    if ((id >> shift) || !out.empty()) {
      out.push_back(static_cast<uint8_t>(id >> shift));
    }
  }
  return out;
}

Bytes Size(uint64_t size) {
  Bytes out = {0x01};
  for (int shift = 48; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(size >> shift));
  }
  return out;
}

Bytes El(uint32_t id, const Bytes& body) {
  return Cat({Id(id), Size(body.size()), body});
}

Bytes Unknown(uint32_t id, const Bytes& body) {
  return Cat({Id(id), kUnknownSize, body});
}

Bytes UInt(uint32_t id, uint64_t value) {
  Bytes body;
  for (int shift = 56; shift >= 0; shift -= 8) {
    body.push_back(static_cast<uint8_t>(value >> shift));
  }
  return El(id, body);
}

Bytes Float(uint32_t id, double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return UInt(id, bits);
}

Bytes Str(uint32_t id, const std::string& text) {
  return El(id, Bytes(text.begin(), text.end()));
}

// A Block or SimpleBlock body: track (below 127), timestamp, flags, data.
Bytes Block(int track, int16_t time, uint8_t flags, const Bytes& data) {
  return Cat({{static_cast<uint8_t>(0x80 | track),
               static_cast<uint8_t>(static_cast<uint16_t>(time) >> 8),
               static_cast<uint8_t>(time), flags},
              data});
}

Bytes Simple(int track, int16_t time, bool keyframe, const Bytes& data) {
  return El(kSimpleBlock, Block(track, time, keyframe ? 0x80 : 0, data));
}

Bytes Cluster(uint64_t time, const Bytes& blocks) {
  return El(kCluster, Cat({UInt(kTimestamp, time), blocks}));
}

Bytes VideoTrack(int number, const std::string& codec) {
  return El(kTrackEntry,
            Cat({UInt(kTrackNumber, number), UInt(kTrackType, 1),
                 Str(kCodecId, codec),
                 El(kVideo, Cat({UInt(kPixelWidth, 64),
                                 UInt(kPixelHeight, 48)}))}));
}

Bytes AudioTrack(int number, const std::string& codec, const Bytes& more = {}) {
  return El(kTrackEntry,
            Cat({UInt(kTrackNumber, number), UInt(kTrackType, 2),
                 Str(kCodecId, codec),
                 El(kAudio, Cat({Float(kSamplingFrequency, 44100),
                                 UInt(kChannels, 2)})),
                 more}));
}

Bytes Header() { return El(kEbml, Str(kDocType, "webm")); }

// Milliseconds per tick.
Bytes Info() {
  return El(kInfo, Cat({UInt(kTimestampScale, 1000000), Float(kDuration, 3000)}));
}

Bytes File(const Bytes& tracks, const Bytes& clusters) {
  return Cat({Header(), El(kSegment, Cat({Info(), El(kTracks, tracks), clusters}))});
}

// Cues after the clusters, found through a SeekHead: each entry is a time
// in ms and the index of its cluster.
Bytes FileWithCues(const Bytes& tracks, const std::vector<Bytes>& clusters,
                   const std::vector<std::pair<uint64_t, size_t>>& cues) {
  auto seek_head = [](uint64_t position) {
    return El(kSeekHead, El(kSeek, Cat({El(kSeekId, Id(kCues)),
                                        UInt(kSeekPosition, position)})));
  };
  const Bytes before = Cat({seek_head(0), Info(), El(kTracks, tracks)});
  std::vector<uint64_t> positions;
  Bytes body;
  for (const Bytes& cluster : clusters) {
    positions.push_back(before.size() + body.size());
    body.insert(body.end(), cluster.begin(), cluster.end());
  }
  Bytes points;
  for (const auto& [time, index] : cues) {
    points = Cat({points,
                  El(kCuePoint,
                     Cat({UInt(kCueTime, time),
                          El(kCueTrackPositions,
                             Cat({UInt(kCueTrack, 1),
                                  UInt(kCueClusterPosition, positions[index])}))}))});
  }
  const uint64_t cues_position = before.size() + body.size();
  return Cat({Header(),
              El(kSegment, Cat({seek_head(cues_position), Info(),
                                El(kTracks, tracks), body, El(kCues, points)}))});
}

class MemorySource : public DataSource {
 public:
  explicit MemorySource(Bytes data) : data_(std::move(data)) {}
  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (offset < 0 || offset >= static_cast<int64_t>(data_.size())) {
      return 0;
    }
    size = std::min(size, data_.size() - static_cast<size_t>(offset));
    memcpy(data, data_.data() + offset, size);
    return static_cast<int64_t>(size);
  }
  int64_t size() const override { return static_cast<int64_t>(data_.size()); }
  std::string error() const override { return std::string(); }

 private:
  const Bytes data_;
};

bool Open(MatroskaDemuxer* demuxer, Bytes file, std::string* error) {
  return demuxer->Open(std::make_unique<MemorySource>(std::move(file)), error);
}

struct Packet {
  int stream;
  int64_t pts;
  int64_t dts;
  int64_t duration;
  bool keyframe;
  Bytes data;
};

struct Packets {
  std::vector<Packet> packets;
  DemuxerReadResult end = DemuxerReadResult::kOk;
};

Packets ReadAll(MatroskaDemuxer* demuxer) {
  Packets result;
  for (int i = 0; i < 1000; ++i) {
    EncodedPacket packet;
    result.end = demuxer->Read(&packet);
    if (result.end != DemuxerReadResult::kOk) {
      break;
    }
    result.packets.push_back(
        {packet.stream_index, packet.pts, packet.dts, packet.duration,
         packet.keyframe,
         Bytes(packet.buffer->data(),
               packet.buffer->data() + packet.buffer->size())});
  }
  return result;
}

Bytes Filled(size_t size, uint8_t value) { return Bytes(size, value); }

TEST(MatroskaDemuxer, RejectsOtherFormats) {
  MatroskaDemuxer demuxer;
  std::string error;
  // An MP4 ftyp box.
  EXPECT_TRUE(!Open(&demuxer, {0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm'},
                    &error));
  EXPECT_EQ(error, std::string(MatroskaDemuxer::kNotMatroska));
  MatroskaDemuxer empty;
  EXPECT_TRUE(!Open(&empty, {}, &error));
  EXPECT_EQ(error, std::string(MatroskaDemuxer::kNotMatroska));
}

TEST(MatroskaDemuxer, Lacing) {
  // Frames of 300, 2 and 5 bytes; 20 ms each.
  const Bytes frames = Cat({Filled(300, 1), Filled(2, 2), Filled(5, 3)});
  const Bytes xiph = Cat({{2, 0xff, 300 - 255, 2}, frames});
  // EBML: the first size, then the difference (2 - 300) as a signed
  // two-byte number: -298 + 8191 = 0x1ED5.
  const Bytes ebml = Cat({{2, 0x41, 0x2c, 0x5e, 0xd5}, frames});
  const Bytes fixed = Cat({{3}, Filled(12, 4)});
  // A first frame larger than the block.
  const Bytes broken = {2, 0xff, 0xff, 0x10, 1, 2, 3};
  const Bytes file = File(
      AudioTrack(1, "A_OPUS", UInt(kDefaultDuration, 20000000)),
      Cluster(0, Cat({El(kSimpleBlock, Block(1, 0, 0x80 | 0x02, xiph)),
                      El(kSimpleBlock, Block(1, 100, 0x80 | 0x06, ebml)),
                      El(kSimpleBlock, Block(1, 200, 0x80 | 0x04, fixed)),
                      El(kSimpleBlock, Block(1, 300, 0x80 | 0x02, broken)),
                      Simple(1, 400, true, {9})})));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  Packets read = ReadAll(&demuxer);
  EXPECT_TRUE(read.end == DemuxerReadResult::kEndOfStream);
  EXPECT_EQ(read.packets.size(), 3u + 3u + 4u + 1u);
  if (read.packets.size() != 11) {
    return;
  }
  for (int lace = 0; lace < 2; ++lace) {
    const Packet* p = &read.packets[lace * 3];
    EXPECT_EQ(p[0].data.size(), 300u);
    EXPECT_TRUE(p[1].data == Bytes(2, 2));
    EXPECT_TRUE(p[2].data == Bytes(5, 3));
    EXPECT_EQ(p[0].pts, lace * 100000);
    EXPECT_EQ(p[1].pts, lace * 100000 + 20000);
    EXPECT_EQ(p[2].pts, lace * 100000 + 40000);
    EXPECT_EQ(p[2].duration, 20000);
  }
  for (int i = 6; i < 10; ++i) {
    EXPECT_TRUE(read.packets[i].data == Bytes(3, 4));
    EXPECT_EQ(read.packets[i].pts, 200000 + (i - 6) * 20000);
  }
  // The malformed block is skipped; the one after it is read.
  EXPECT_EQ(read.packets[10].pts, 400000);
}

TEST(MatroskaDemuxer, BlockGroups) {
  // Keyframes are the blocks without a ReferenceBlock; BlockDuration is
  // the frame's duration.
  const Bytes file = File(
      VideoTrack(1, "V_VP9"),
      Cluster(1000, Cat({El(kBlockGroup, Cat({El(kBlock, Block(1, 0, 0, {1})),
                                              UInt(kBlockDuration, 40)})),
                         El(kBlockGroup, Cat({El(kBlock, Block(1, 40, 0, {2})),
                                              UInt(kReferenceBlock, 0xffd8),
                                              UInt(kBlockDuration, 40)}))})));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  EXPECT_EQ(demuxer.media_info().start_time, 1000000);
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 2u);
  if (read.packets.size() == 2) {
    EXPECT_TRUE(read.packets[0].keyframe);
    EXPECT_TRUE(!read.packets[1].keyframe);
    EXPECT_EQ(read.packets[0].pts, 1000000);
    EXPECT_EQ(read.packets[1].pts, 1040000);
    EXPECT_EQ(read.packets[1].duration, 40000);
    EXPECT_TRUE(read.packets[1].data == Bytes{2});
  }
}

TEST(MatroskaDemuxer, HeaderStripping) {
  auto encoding = [](uint64_t algorithm) {
    return El(kContentEncodings,
              El(kContentEncoding,
                 El(kContentCompression,
                    Cat({UInt(kContentCompAlgo, algorithm),
                         El(kContentCompSettings, {0xaa, 0xbb})}))));
  };
  // Track 2 is zlib-compressed, which this build cannot read: skipped.
  const Bytes file = File(
      Cat({AudioTrack(1, "A_MPEG/L3",
                      Cat({encoding(3), UInt(kDefaultDuration, 26000000)})),
           AudioTrack(2, "A_MPEG/L3", encoding(0))}),
      Cluster(0, Cat({Simple(1, 0, true, {1, 2}),
                      El(kSimpleBlock, Block(1, 26, 0x84, {1, 3, 4}))})));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  EXPECT_EQ(demuxer.media_info().streams.size(), 1u);
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 3u);
  if (read.packets.size() == 3) {
    EXPECT_TRUE(read.packets[0].data == (Bytes{0xaa, 0xbb, 1, 2}));
    EXPECT_TRUE(read.packets[1].data == (Bytes{0xaa, 0xbb, 3}));
    EXPECT_TRUE(read.packets[2].data == (Bytes{0xaa, 0xbb, 4}));
  }
}

TEST(MatroskaDemuxer, UnknownSizeClusters) {
  // As live recorders write: the segment and each cluster end where the
  // next one starts. Seeks find the clusters by walking their children.
  const Bytes file = Cat(
      {Header(),
       Unknown(kSegment,
               Cat({Info(), El(kTracks, VideoTrack(1, "V_VP9")),
                    Unknown(kCluster, Cat({UInt(kTimestamp, 0),
                                           Simple(1, 0, true, {1}),
                                           Simple(1, 500, false, {2})})),
                    Unknown(kCluster, Cat({UInt(kTimestamp, 1000),
                                           Simple(1, 0, true, {3}),
                                           Simple(1, 500, false, {4})}))}))});
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  Packets read = ReadAll(&demuxer);
  EXPECT_TRUE(read.end == DemuxerReadResult::kEndOfStream);
  EXPECT_EQ(read.packets.size(), 4u);
  if (read.packets.size() == 4) {
    EXPECT_EQ(read.packets[2].pts, 1000000);
    EXPECT_EQ(read.packets[3].pts, 1500000);
  }
  EXPECT_TRUE(demuxer.Seek(1200000));
  read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 2u);
  if (!read.packets.empty()) {
    EXPECT_EQ(read.packets[0].pts, 1000000);
  }
}

// Clusters at 0, 1 and 2 s. The one at 1 s starts with a frame that is
// not a keyframe, which a seek into it skips.
std::vector<Bytes> SeekClusters() {
  return {Cluster(0, Cat({Simple(1, 0, true, {1}), Simple(1, 100, false, {2})})),
          Cluster(1000, Cat({Simple(1, 0, false, {3}), Simple(1, 200, true, {4})})),
          Cluster(2000, Cat({Simple(1, 0, true, {5}), Simple(1, 100, false, {6})}))};
}

TEST(MatroskaDemuxer, SeeksThroughCues) {
  // The Cues list only the clusters at 0 and 2 s, so a seek to 1.5 s goes
  // back to 0 s if they are used.
  const Bytes file = FileWithCues(VideoTrack(1, "V_VP9"), SeekClusters(),
                                  {{0, 0}, {2000, 2}});
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  EXPECT_TRUE(demuxer.media_info().seekable);
  EXPECT_TRUE(demuxer.Seek(1500000));
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 6u);
  if (!read.packets.empty()) {
    EXPECT_EQ(read.packets[0].pts, 0);
  }
  EXPECT_TRUE(demuxer.Seek(2500000));
  read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 2u);
  if (!read.packets.empty()) {
    EXPECT_EQ(read.packets[0].pts, 2000000);
  }
}

TEST(MatroskaDemuxer, SeeksScanningClusters) {
  std::vector<Bytes> clusters = SeekClusters();
  const Bytes file = File(VideoTrack(1, "V_VP9"),
                          Cat({clusters[0], clusters[1], clusters[2]}));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  EXPECT_TRUE(demuxer.Seek(1500000));
  Packets read = ReadAll(&demuxer);
  // The cluster at 1 s, from its keyframe.
  EXPECT_EQ(read.packets.size(), 3u);
  if (!read.packets.empty()) {
    EXPECT_EQ(read.packets[0].pts, 1200000);
    EXPECT_TRUE(read.packets[0].keyframe);
  }
  // Before the first cluster: from the start.
  EXPECT_TRUE(demuxer.Seek(0));
  EXPECT_EQ(ReadAll(&demuxer).packets.size(), 6u);
}

TEST(MatroskaDemuxer, DtsOfReorderedFrames) {
  // H.264 in decode order I P B B P; Matroska stores only the pts.
  const Bytes file = File(
      VideoTrack(1, "V_MPEG4/ISO/AVC"),
      Cluster(0, Cat({Simple(1, 0, true, {1}), Simple(1, 100, false, {2}),
                      Simple(1, 33, false, {3}), Simple(1, 67, false, {4}),
                      Simple(1, 133, false, {5})})));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  Packets read = ReadAll(&demuxer);
  const std::vector<int64_t> expected = {0, 33000, 33000, 67000, 133000};
  EXPECT_EQ(read.packets.size(), expected.size());
  for (size_t i = 0; i < read.packets.size() && i < expected.size(); ++i) {
    EXPECT_EQ(read.packets[i].dts, expected[i]);
    EXPECT_TRUE(read.packets[i].dts <= read.packets[i].pts);
  }
}

TEST(MatroskaDemuxer, CodecConfiguration) {
  const Bytes vp9 = El(
      kTrackEntry,
      Cat({UInt(kTrackNumber, 1), UInt(kTrackType, 1), Str(kCodecId, "V_VP9"),
           El(kVideo, Cat({UInt(kPixelWidth, 640), UInt(kPixelHeight, 480),
                           UInt(kDisplayWidth, 4), UInt(kDisplayHeight, 3),
                           El(kColour, Cat({UInt(kMatrixCoefficients, 9),
                                            UInt(kRange, 2),
                                            UInt(kTransferCharacteristics, 16),
                                            UInt(kPrimaries, 9)}))}))}));
  // Opus: 6.5 ms of codec delay comes off the timestamps. AAC without a
  // CodecPrivate gets an AudioSpecificConfig: LC, 44.1 kHz, stereo.
  const Bytes file = File(
      Cat({vp9, AudioTrack(2, "A_OPUS", UInt(kCodecDelay, 6500000)),
           AudioTrack(3, "A_AAC/MPEG4/LC")}),
      Cluster(0, Simple(2, 0, true, {0xfc})));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  const MediaInfo& info = demuxer.media_info();
  EXPECT_EQ(info.streams.size(), 3u);
  if (info.streams.size() != 3) {
    return;
  }
  const VideoConfig& video = info.streams[0].video;
  EXPECT_TRUE(video.codec == VideoCodec::kVP9);
  EXPECT_EQ(int{video.color.matrix}, 9);
  EXPECT_EQ(int{video.color.transfer}, 16);
  EXPECT_EQ(int{video.color.primaries}, 9);
  EXPECT_TRUE(video.color.full_range);
  // 4:3 at 640x480: square pixels.
  EXPECT_EQ(video.sar_num, 1);
  EXPECT_EQ(video.sar_den, 1);
  EXPECT_TRUE(info.streams[1].audio.codec == AudioCodec::kOpus);
  EXPECT_EQ(info.streams[1].audio.sample_rate, 48000);
  EXPECT_TRUE(info.streams[2].audio.extradata == (Bytes{0x12, 0x10}));
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 1u);
  if (!read.packets.empty()) {
    EXPECT_EQ(read.packets[0].pts, -6500);
  }
}

TEST(MatroskaDemuxer, WebVttCueLines) {
  // WebM's WebVTT: the cue's identifier and settings lines come first.
  const Bytes subtitle = El(
      kTrackEntry, Cat({UInt(kTrackNumber, 1), UInt(kTrackType, 0x11),
                        Str(kCodecId, "D_WEBVTT/SUBTITLES")}));
  const std::string cue = "intro\nline:0\nHello\nthere";
  const Bytes file = File(
      subtitle,
      Cluster(0, El(kBlockGroup, Cat({El(kBlock, Block(1, 1000, 0,
                                                      Bytes(cue.begin(), cue.end()))),
                                      UInt(kBlockDuration, 1500)}))));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  EXPECT_EQ(demuxer.media_info().streams[0].subtitle.codec_name,
            std::string("webvtt"));
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 1u);
  if (!read.packets.empty()) {
    const std::string text(read.packets[0].data.begin(),
                           read.packets[0].data.end());
    EXPECT_EQ(text, std::string("Hello\nthere"));
    EXPECT_EQ(read.packets[0].pts, 1000000);
    EXPECT_EQ(read.packets[0].duration, 1500000);
  }
}

TEST(MatroskaDemuxer, SizeBounds) {
  // A Tracks element claiming 1 GiB is an error, not an allocation.
  Bytes huge_tracks = Cat({Header(), Unknown(kSegment, Cat({Info(), Id(kTracks),
                                                            Size(1 << 30),
                                                            VideoTrack(1, "V_VP9")}))});
  MatroskaDemuxer tracks;
  std::string error;
  EXPECT_TRUE(!Open(&tracks, huge_tracks, &error));
  EXPECT_TRUE(error.find("too large") != std::string::npos);

  // So is a block of 1 GiB in a cluster of unknown size.
  const Bytes huge_block = Cat(
      {Header(),
       Unknown(kSegment,
               Cat({Info(), El(kTracks, VideoTrack(1, "V_VP9")),
                    Unknown(kCluster, Cat({UInt(kTimestamp, 0),
                                           Simple(1, 0, true, {1}), Id(kSimpleBlock),
                                           Size(1 << 30), Block(1, 40, 0x80, {2})}))}))});
  MatroskaDemuxer block;
  EXPECT_TRUE(Open(&block, huge_block, &error));
  Packets read = ReadAll(&block);
  EXPECT_EQ(read.packets.size(), 1u);
  EXPECT_TRUE(read.end == DemuxerReadResult::kError);
  EXPECT_TRUE(block.error().find("too large") != std::string::npos);
}

TEST(MatroskaDemuxer, TruncatedFile) {
  Bytes file = File(VideoTrack(1, "V_VP9"),
                    Cluster(0, Cat({Simple(1, 0, true, Filled(100, 1)),
                                    Simple(1, 40, false, Filled(100, 2))})));
  file.resize(file.size() - 50);
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 1u);
  EXPECT_TRUE(read.end == DemuxerReadResult::kError);
  EXPECT_TRUE(demuxer.error().find("input ended") != std::string::npos);
}

TEST(MatroskaDemuxer, ResyncsAtTheNextCluster) {
  // Damage between two clusters: reading goes on at the second.
  const Bytes file = File(
      VideoTrack(1, "V_VP9"),
      Cat({Cluster(0, Simple(1, 0, true, {1})), Filled(100, 0),
           Cluster(1000, Simple(1, 0, true, {2}))}));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  Packets read = ReadAll(&demuxer);
  EXPECT_TRUE(read.end == DemuxerReadResult::kEndOfStream);
  EXPECT_EQ(read.packets.size(), 2u);
  if (read.packets.size() == 2) {
    EXPECT_EQ(read.packets[1].pts, 1000000);
  }
}

TEST(MatroskaDemuxer, EnabledStreams) {
  const Bytes file = File(
      Cat({VideoTrack(1, "V_VP9"), AudioTrack(2, "A_OPUS")}),
      Cluster(0, Cat({Simple(1, 0, true, {1}), Simple(2, 0, true, {2}),
                      Simple(1, 40, false, {3}), Simple(2, 20, true, {4})})));
  MatroskaDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(Open(&demuxer, file, &error));
  demuxer.SetEnabledStreams({1});
  Packets read = ReadAll(&demuxer);
  EXPECT_EQ(read.packets.size(), 2u);
  for (const Packet& packet : read.packets) {
    EXPECT_EQ(packet.stream, 1);
  }
}

}  // namespace
}  // namespace lychee
