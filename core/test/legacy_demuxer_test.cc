#include "backends/avi/avi_demuxer.h"
#include "backends/flv/flv_demuxer.h"
#include "backends/mpegps/ps_demuxer.h"
#include "test.h"

#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

// NOLINTBEGIN(bugprone-throwing-static-initialization)
namespace lychee {
namespace {
using Bytes = std::vector<uint8_t>;
void PutBe(Bytes* out, uint32_t value, int width) {
  for (int i = width - 1; i >= 0; --i)
    out->push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void FlvTag(Bytes* out, uint8_t type, uint32_t ms, const Bytes& payload) {
  out->push_back(type);
  PutBe(out, static_cast<uint32_t>(payload.size()), 3);
  PutBe(out, ms & 0xffffff, 3);
  out->push_back(static_cast<uint8_t>(ms >> 24));
  PutBe(out, 0, 3);
  out->insert(out->end(), payload.begin(), payload.end());
  PutBe(out, static_cast<uint32_t>(payload.size()) + 11, 4);
}
Bytes FlvAudio() {
  Bytes bytes = {'F', 'L', 'V', 1, 4, 0, 0, 0, 9, 0, 0, 0, 0};
  FlvTag(&bytes, 8, 0, {0xaf, 0, 0x11, 0x90});
  for (uint32_t ms : {0u, 21u, 42u})
    FlvTag(&bytes, 8, ms, {0xaf, 1, 1, 2, 3});
  return bytes;
}
TEST(FlvDemuxer, AudioPacketsAndSeek) {
  FlvDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(demuxer.Open(CreateMemoryDataSource(FlvAudio()), &error));
  EXPECT_EQ(demuxer.media_info().streams.size(), 1u);
  EXPECT_EQ(demuxer.media_info().streams[0].audio.sample_rate, 48000);
  EncodedPacket packet;
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 0);
  EXPECT_EQ(packet.duration, 21000);
  EXPECT_EQ(packet.size(), 3u);
  EXPECT_TRUE(demuxer.Seek(30000));
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 21000);
  demuxer.SetEnabledStreams({});
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kEndOfStream);
  demuxer.Abort();
  EXPECT_TRUE(!demuxer.Seek(0));
}
TEST(FlvDemuxer, RejectsTruncationAndBadPreviousSize) {
  for (bool truncate : {false, true}) {
    auto bytes = FlvAudio();
    if (truncate)
      bytes.pop_back();
    else
      bytes.back() ^= 1;
    FlvDemuxer demuxer;
    std::string error;
    EXPECT_TRUE(
        !demuxer.Open(CreateMemoryDataSource(std::move(bytes)), &error));
  }
}
Bytes FlvHeader(uint8_t flags) {
  return {'F', 'L', 'V', 1, flags, 0, 0, 0, 9, 0, 0, 0, 0};
}
// AMF0 onMetaData with numbers.
Bytes FlvMetadata(
    std::initializer_list<std::pair<const char*, double>> values) {
  Bytes out = {2, 0, 10};
  for (char c : std::string("onMetaData"))
    out.push_back(uint8_t(c));
  out.push_back(8);
  PutBe(&out, static_cast<uint32_t>(values.size()) + 1, 4);
  // A string first, which the reader skips.
  for (uint8_t c : Bytes{0, 7, 'e', 'n', 'c', 'o', 'd', 'e', 'r', 2, 0, 1, 'x'})
    out.push_back(c);
  for (const auto& [name, value] : values) {
    PutBe(&out, static_cast<uint32_t>(strlen(name)), 2);
    out.insert(out.end(), name, name + strlen(name));
    out.push_back(0);
    uint64_t bits;
    memcpy(&bits, &value, 8);
    for (int i = 7; i >= 0; --i)
      out.push_back(uint8_t(bits >> (i * 8)));
  }
  PutBe(&out, 0, 2);
  out.push_back(9);
  return out;
}
const Bytes kHvcC = [] {
  Bytes hvcc(23);
  hvcc[0] = 1;
  hvcc[21] = 3;
  return hvcc;
}();
TEST(FlvDemuxer, HevcAndAv1Headers) {
  // Codec id 12, and Enhanced RTMP's hvc1 and av01.
  struct Case {
    Bytes config;
    Bytes frame;
    VideoCodec codec;
    int64_t pts;
  };
  Bytes legacy_config = {0x1c, 0, 0, 0, 0};
  legacy_config.insert(legacy_config.end(), kHvcC.begin(), kHvcC.end());
  Bytes enhanced_config = {0x90, 'h', 'v', 'c', '1'};
  enhanced_config.insert(enhanced_config.end(), kHvcC.begin(), kHvcC.end());
  const Case cases[] = {
      {legacy_config, {0x1c, 1, 0, 0, 40, 7}, VideoCodec::kHEVC, 40},
      {enhanced_config,
       {0x91, 'h', 'v', 'c', '1', 0, 0, 40, 7},
       VideoCodec::kHEVC,
       40},
      // CodedFramesX: no composition time.
      {enhanced_config, {0x93, 'h', 'v', 'c', '1', 7}, VideoCodec::kHEVC, 0},
      {{0x90, 'a', 'v', '0', '1', 0x81, 0, 0, 0},
       {0x91, 'a', 'v', '0', '1', 7},
       VideoCodec::kAV1,
       0},
  };
  for (const Case& c : cases) {
    {
      Bytes bytes = FlvHeader(1);
      FlvTag(&bytes, 18, 0, FlvMetadata({{"width", 1280}, {"height", 720}}));
      FlvTag(&bytes, 9, 0, c.config);
      // A command frame, skipped.
      FlvTag(&bytes, 9, 0, {0xd0, 'h', 'v', 'c', '1', 0});
      FlvTag(&bytes, 9, 0, c.frame);
      FlvDemuxer demuxer;
      std::string error;
      EXPECT_TRUE(demuxer.Open(CreateMemoryDataSource(bytes), &error));
      const MediaInfo& info = demuxer.media_info();
      EXPECT_EQ(info.streams.size(), 1u);
      EXPECT_TRUE(info.streams[0].video.codec == c.codec);
      EXPECT_EQ(info.streams[0].video.width, 1280);
      EncodedPacket packet;
      EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
      EXPECT_EQ(packet.pts, c.pts * 1000);
      EXPECT_TRUE(packet.keyframe);
      EXPECT_EQ(packet.size(), 1u);
      EXPECT_EQ(packet.buffer->data()[0], 7);
    }
  }
}
void PutLe(Bytes* out, uint32_t value, int width) {
  for (int i = 0; i < width; ++i)
    out->push_back(static_cast<uint8_t>(value >> (i * 8)));
}
Bytes RiffChunk(const char* type, const Bytes& payload) {
  Bytes out(type, type + 4);
  PutLe(&out, static_cast<uint32_t>(payload.size()), 4);
  out.insert(out.end(), payload.begin(), payload.end());
  if (payload.size() & 1) out.push_back(0);
  return out;
}
Bytes Join(Bytes a, const Bytes& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}
TEST(AviDemuxer, PcmClockAndSeek) {
  Bytes strh(48);
  strh[0] = 'a';
  strh[1] = 'u';
  strh[2] = 'd';
  strh[3] = 's';
  strh[20] = 1;
  strh[24] = 0x80;
  strh[25] = 0xbb;
  strh[44] = 2;
  Bytes fmt;
  PutLe(&fmt, 1, 2);
  PutLe(&fmt, 1, 2);
  PutLe(&fmt, 48000, 4);
  PutLe(&fmt, 96000, 4);
  PutLe(&fmt, 2, 2);
  PutLe(&fmt, 16, 2);
  Bytes strl = {'s', 't', 'r', 'l'};
  strl = Join(strl, RiffChunk("strh", strh));
  strl = Join(strl, RiffChunk("strf", fmt));
  Bytes hdrl = {'h', 'd', 'r', 'l'};
  hdrl = Join(hdrl, RiffChunk("LIST", strl));
  Bytes movie = {'m', 'o', 'v', 'i'};
  movie = Join(movie, RiffChunk("00wb", Bytes(4096, 1)));
  Bytes body = {'A', 'V', 'I', ' '};
  body = Join(body, RiffChunk("LIST", hdrl));
  body = Join(body, RiffChunk("LIST", movie));
  AviDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(
      demuxer.Open(CreateMemoryDataSource(RiffChunk("RIFF", body)), &error));
  EXPECT_EQ(demuxer.media_info().streams.size(), 1u);
  EncodedPacket packet;
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 0);
  EXPECT_EQ(packet.size(), 2048u);
  EXPECT_TRUE(demuxer.Seek(30000));
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 21333);
}
TEST(AviDemuxer, RejectsPacketsBeforeStreamFormat) {
  Bytes strh(48);
  memcpy(strh.data(), "auds", 4);
  strh[20] = 1;
  strh[24] = 0x80;
  strh[25] = 0xbb;
  Bytes strl = {'s', 't', 'r', 'l'};
  strl = Join(strl, RiffChunk("strh", strh));
  Bytes hdrl = {'h', 'd', 'r', 'l'};
  hdrl = Join(hdrl, RiffChunk("LIST", strl));
  Bytes movie = {'m', 'o', 'v', 'i'};
  movie = Join(movie, RiffChunk("00wb", Bytes(4, 1)));
  Bytes body = {'A', 'V', 'I', ' '};
  body = Join(body, RiffChunk("LIST", hdrl));
  body = Join(body, RiffChunk("LIST", movie));
  AviDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(
      !demuxer.Open(CreateMemoryDataSource(RiffChunk("RIFF", body)), &error));
  EXPECT_EQ(error, "avi: packet before stream format");
}

TEST(AviDemuxer, EmptyVideoChunkAdvancesClock) {
  Bytes strh(48);
  memcpy(strh.data(), "vids", 4);
  strh[20] = 1;
  strh[24] = 25;
  Bytes format(40);
  format[0] = 40;
  format[4] = 16;
  format[8] = 16;
  memcpy(format.data() + 16, "H264", 4);
  Bytes strl = {'s', 't', 'r', 'l'};
  strl = Join(strl, RiffChunk("strh", strh));
  strl = Join(strl, RiffChunk("strf", format));
  Bytes hdrl = {'h', 'd', 'r', 'l'};
  hdrl = Join(hdrl, RiffChunk("LIST", strl));
  Bytes movie = {'m', 'o', 'v', 'i'};
  movie = Join(movie, RiffChunk("00dc", {0, 0, 0, 1, 0x65, 0x88, 0}));
  movie = Join(movie, RiffChunk("00dc", {}));
  movie = Join(movie, RiffChunk("00dc", {0, 0, 0, 1, 0x41, 0x88, 0}));
  Bytes body = {'A', 'V', 'I', ' '};
  body = Join(body, RiffChunk("LIST", hdrl));
  body = Join(body, RiffChunk("LIST", movie));
  AviDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(
      demuxer.Open(CreateMemoryDataSource(RiffChunk("RIFF", body)), &error));
  EncodedPacket packet;
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.dts, 0);
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.dts, 80000);
}
// Three deliberately non-IDR samples. Only the index marks samples 0 and 2
// as sync samples, so seek assertions exercise the index rather than NAL
// parsing.
Bytes IndexedAvi(bool absolute, bool open_dml, bool bad_offset = false) {
  Bytes strh(48);
  memcpy(strh.data(), "vids", 4);
  strh[20] = 1;
  strh[24] = 25;
  Bytes format(40);
  format[0] = 40;
  format[4] = 16;
  format[8] = 16;
  memcpy(format.data() + 16, "H264", 4);
  Bytes strl = {'s', 't', 'r', 'l'};
  strl = Join(strl, RiffChunk("strh", strh));
  strl = Join(strl, RiffChunk("strf", format));
  // OpenDML's super index lives inside the stream list. Patch its pointer
  // after placing the standard index after movi.
  size_t super_entry = 0;
  if (open_dml) {
    Bytes super;
    PutLe(&super, 4, 2);
    super.insert(super.end(), {0, 0});
    PutLe(&super, 1, 4);
    super.insert(super.end(), {'0', '0', 'd', 'c'});
    super.resize(40);
    super_entry = 8 + 4 + 8 + 4 + 8 + strl.size() + 8 + 24;
    strl = Join(strl, RiffChunk("indx", super));
  }
  Bytes hdrl = {'h', 'd', 'r', 'l'};
  hdrl = Join(hdrl, RiffChunk("LIST", strl));
  Bytes body = {'A', 'V', 'I', ' '};
  body = Join(body, RiffChunk("LIST", hdrl));
  const uint32_t movi_type = static_cast<uint32_t>(8 + body.size() + 8);
  Bytes movie = {'m', 'o', 'v', 'i'};
  for (int n = 0; n < 3; ++n)
    movie = Join(movie, RiffChunk("00dc", {0, 0, 0, 1, 0x41, 0x88, 0}));
  body = Join(body, RiffChunk("LIST", movie));
  if (open_dml) {
    const uint32_t index_offset = static_cast<uint32_t>(8 + body.size());
    Bytes standard;
    PutLe(&standard, 2, 2);
    standard.insert(standard.end(), {0, 1});
    PutLe(&standard, 3, 4);
    standard.insert(standard.end(), {'0', '0', 'd', 'c'});
    PutLe(&standard, movi_type + 4, 4);
    PutLe(&standard, 0, 4);
    PutLe(&standard, 0, 4);
    for (uint32_t n = 0; n < 3; ++n) {
      PutLe(&standard, 8 + n * 16, 4);
      PutLe(&standard, 7 | (n == 1 ? 0x80000000u : 0u), 4);
    }
    body = Join(body, RiffChunk("ix00", standard));
    // super_entry is an absolute file position; body begins at byte 8.
    Bytes entry;
    PutLe(&entry, bad_offset ? 0xfffffff0u : index_offset, 4);
    PutLe(&entry, 0, 4);
    PutLe(&entry, static_cast<uint32_t>(standard.size() + 8), 4);
    PutLe(&entry, 3, 4);
    std::copy(entry.begin(), entry.end(),
              body.begin() + static_cast<ptrdiff_t>(super_entry - 8));
  } else {
    Bytes index;
    for (uint32_t n = 0; n < 3; ++n) {
      index.insert(index.end(), {'0', '0', 'd', 'c'});
      PutLe(&index, n == 1 ? 0 : 0x10, 4);
      PutLe(&index,
            bad_offset ? 0xfffffff0u : (absolute ? movi_type : 0) + 4 + n * 16,
            4);
      PutLe(&index, 7, 4);
    }
    body = Join(body, RiffChunk("idx1", index));
  }
  return RiffChunk("RIFF", body);
}
void CheckIndexedAvi(Bytes bytes) {
  AviDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(demuxer.Open(CreateMemoryDataSource(std::move(bytes)), &error));
  EncodedPacket packet;
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_TRUE(packet.keyframe);
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_TRUE(!packet.keyframe);
  EXPECT_TRUE(demuxer.Seek(60000));
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 0);
  EXPECT_TRUE(demuxer.Seek(100000));
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 80000);
  EXPECT_TRUE(packet.keyframe);
}
TEST(AviDemuxer, Idx1RelativeAndAbsoluteOffsetsControlSeek) {
  CheckIndexedAvi(IndexedAvi(false, false));
  CheckIndexedAvi(IndexedAvi(true, false));
}
TEST(AviDemuxer, OpenDmlSuperAndStandardIndexesControlSeek) {
  CheckIndexedAvi(IndexedAvi(false, true));
}
TEST(AviDemuxer, RejectsOutOfBoundsIndexOffsets) {
  for (bool open_dml : {false, true}) {
    AviDemuxer demuxer;
    std::string error;
    EXPECT_TRUE(!demuxer.Open(
        CreateMemoryDataSource(IndexedAvi(false, open_dml, true)), &error));
  }
}
void PsPes(Bytes* out, const Bytes& payload, bool timed = false) {
  out->insert(out->end(), {0, 0, 1, 0xe0});
  PutBe(out, static_cast<uint32_t>(payload.size() + (timed ? 8 : 3)), 2);
  if (timed)
    out->insert(out->end(), {0x80, 0x80, 5, 0x21, 0, 3, 0x5f, 0x91});
  else
    out->insert(out->end(), {0x80, 0, 0});
  out->insert(out->end(), payload.begin(), payload.end());
}
TEST(PsDemuxer, PicturesCrossPesAndSeekRestoresSequence) {
  Bytes out = {0, 0, 1, 0xba, 0x44, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const Bytes sequence = {0, 0, 1, 0xb3, 0x2d, 0x01, 0xe0, 0x33};
  Bytes elementary = sequence;
  elementary.insert(elementary.end(), {0, 0, 1, 0, 0, 8, 0x11, 0x22, 0, 0, 1, 0,
                                       0, 0x50, 0x33, 0x44});
  PsPes(&out, Bytes(elementary.begin(), elementary.begin() + 13), true);
  PsPes(&out, Bytes(elementary.begin() + 13, elementary.end()));
  PsDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(demuxer.Open(CreateMemoryDataSource(std::move(out)), &error));
  EXPECT_EQ(demuxer.media_info().streams[0].video.width, 720);
  EXPECT_EQ(demuxer.media_info().start_time, 500000);
  EXPECT_EQ(demuxer.media_info().duration, 80000);
  EncodedPacket packet;
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_TRUE(packet.keyframe);
  EXPECT_EQ(packet.pts, 500000);
  EXPECT_TRUE(packet.size() >= sequence.size());
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 540000);
  EXPECT_TRUE(demuxer.Seek(560000));
  EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_TRUE(packet.keyframe);
  EXPECT_EQ(packet.pts, 500000);
}

}  // namespace
}  // namespace lychee
// NOLINTEND(bugprone-throwing-static-initialization)
