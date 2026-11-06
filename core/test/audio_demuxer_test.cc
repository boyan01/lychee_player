#include <bit>
#include <cmath>
#include <cstring>
#include <vector>
#include "backends/audio/adts_demuxer.h"
#include "backends/audio/flac_demuxer.h"
#include "backends/audio/mp3_demuxer.h"
#include "backends/audio/ogg_demuxer.h"
#include "backends/audio/wav_demuxer.h"
#include "media/pcm_audio_decoder.h"
#include "test.h"

// NOLINTBEGIN(bugprone-throwing-static-initialization)
namespace lychee {
namespace {
using Bytes = std::vector<uint8_t>;
void LE(Bytes* out, uint64_t value, int count) {
  for (int i = 0; i < count; ++i)
    out->push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void Four(Bytes* out, const char* text) {
  out->insert(out->end(), text, text + 4);
}
Bytes Wave(unsigned tag,
           unsigned bits,
           unsigned channels,
           Bytes samples,
           uint32_t mask = 0) {
  Bytes body;
  Four(&body, "WAVE");
  Four(&body, "fmt ");
  LE(&body, tag == 0xfffe ? 40 : 16, 4);
  LE(&body, tag, 2);
  LE(&body, channels, 2);
  LE(&body, 48000, 4);
  LE(&body, uint64_t{48000} * channels * (bits / 8), 4);
  LE(&body, uint64_t{channels} * (bits / 8), 2);
  LE(&body, bits, 2);
  if (tag == 0xfffe) {
    LE(&body, 22, 2);
    LE(&body, bits, 2);
    LE(&body, mask, 4);
    LE(&body, 1, 4);
    LE(&body, 0, 2);
    LE(&body, 0x10, 2);
    body.insert(body.end(), {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71});
  }
  Four(&body, "data");
  LE(&body, samples.size(), 4);
  body.insert(body.end(), samples.begin(), samples.end());
  Bytes out;
  Four(&out, "RIFF");
  LE(&out, body.size(), 4);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
Bytes Page(uint32_t sequence,
           uint8_t flags,
           uint64_t granule,
           Bytes laces,
           Bytes payload) {
  Bytes p = {'O', 'g', 'g', 'S', 0, flags};
  LE(&p, granule, 8);
  LE(&p, 7, 4);
  LE(&p, sequence, 4);
  LE(&p, 0, 4);
  p.push_back(static_cast<uint8_t>(laces.size()));
  p.insert(p.end(), laces.begin(), laces.end());
  p.insert(p.end(), payload.begin(), payload.end());
  uint32_t crc = 0;
  for (uint8_t byte : p) {
    crc ^= uint32_t(byte) << 24;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
  }
  for (int i = 0; i < 4; ++i)
    p[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
  return p;
}
void Append(Bytes* a, const Bytes& b) {
  a->insert(a->end(), b.begin(), b.end());
}
Bytes OpusOgg(uint64_t granule = 960) {
  Bytes head = {'O', 'p', 'u',  's',  'H', 'e', 'a', 'd', 1, 1,
                0,   0,   0x80, 0xbb, 0,   0,   0,   0,   0};
  Bytes out = Page(0, 2, 0, {19}, head);
  Append(&out, Page(1, 0, 0, {8}, {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'}));
  Append(&out, Page(2, 4, granule, {2}, {0xf8, 0}));
  return out;
}
TEST(Audio, WavPcmAndSeek) {
  WavDemuxer d;
  std::string error;
  Bytes samples(size_t{48000} * 2, 0);
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(Wave(1, 16, 1, samples)), &error));
  EXPECT_EQ(d.media_info().duration, 1000000);
  EXPECT_TRUE(d.Seek(500000));
  EncodedPacket p;
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 500000);
  d.SetEnabledStreams({});
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kEndOfStream);
}
TEST(Audio, WavRejectsInvalidExtensibleLayoutAndTruncation) {
  std::string error;
  WavDemuxer bad_layout;
  EXPECT_TRUE(!bad_layout.Open(
      CreateMemoryDataSource(Wave(0xfffe, 16, 2, Bytes(4), 0x30)), &error));
  WavDemuxer good;
  EXPECT_TRUE(good.Open(
      CreateMemoryDataSource(Wave(0xfffe, 24, 2, Bytes(6), 3)), &error));
  Bytes truncated = Wave(1, 16, 1, Bytes(8));
  truncated.pop_back();
  WavDemuxer bad_data;
  EXPECT_TRUE(
      !bad_data.Open(CreateMemoryDataSource(std::move(truncated)), &error));
}
TEST(Audio, Mp3Id3AndXingRestart) {
  Bytes frame(417, 0);
  frame[0] = 0xff;
  frame[1] = 0xfb;
  frame[2] = 0x90;
  frame[3] = 0;
  memcpy(frame.data() + 36, "Xing", 4);
  frame[43] = 1;
  frame[47] = 2;
  Bytes audio = frame;
  audio[100] = 42;
  memcpy(audio.data() + 36, "data", 4);
  Bytes file = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
  Append(&file, frame);
  Append(&file, audio);
  Mp3Demuxer d;
  std::string error;
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(file), &error));
  EncodedPacket p;
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.buffer->data()[100], 42);
  EXPECT_TRUE(d.Seek(0));
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.buffer->data()[100], 42);
  EXPECT_EQ(p.pts, 0);
  file[6] = 0x80;
  Mp3Demuxer invalid;
  EXPECT_TRUE(!invalid.Open(CreateMemoryDataSource(file), &error));
}
TEST(Audio, Mp3GaplessEncoderDelayAndPadding) {
  Bytes frame(417, 0);
  frame[0] = 0xff;
  frame[1] = 0xfb;
  frame[2] = 0x90;
  memcpy(frame.data() + 36, "Xing", 4);
  frame[43] = 1;
  frame[47] = 100;
  memcpy(frame.data() + 48, "Lavc", 4);
  frame[69] = 0x24;
  frame[70] = 0x02;
  frame[71] = 0x58;
  Bytes audio = frame;
  memcpy(audio.data() + 36, "data", 4);
  Append(&frame, audio);
  Mp3Demuxer d;
  std::string error;
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(frame), &error));
  EXPECT_EQ(d.media_info().start_time, 1105 * int64_t(1000000) / 44100);
  EXPECT_EQ(d.media_info().duration,
            (100 * 1152 - 576 - 600) * int64_t(1000000) / 44100);
  EXPECT_TRUE(d.Seek(d.media_info().start_time));
  EncodedPacket p;
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 0);
}
TEST(Audio, Mp3VbriTableSeeksToFrameSpan) {
  Bytes frame(417, 0);
  frame[0] = 0xff;
  frame[1] = 0xfb;
  frame[2] = 0x90;
  memcpy(frame.data() + 36, "VBRI", 4);
  const auto put = [&](size_t at, uint32_t value, int width) {
    for (int i = width - 1; i >= 0; --i) {
      frame[at++] = static_cast<uint8_t>(value >> (i * 8));
    }
  };
  put(40, 1, 2);  // VBRI version.
  put(46, 40 * 417, 4);
  put(50, 40, 4);  // Forty MPEG-1 audio frames.
  put(54, 4, 2);   // Four seek entries.
  put(56, 3, 2);   // Byte scale.
  put(58, 2, 2);   // Two bytes per entry.
  put(60, 10, 2);  // Ten frames per entry.
  for (size_t at = 62; at < 70; at += 2)
    put(at, 1390, 2);
  Bytes file = frame;
  for (uint8_t i = 1; i < 40; ++i) {
    memcpy(frame.data() + 36, "data", 4);
    frame[100] = i;
    Append(&file, frame);
  }
  Mp3Demuxer d;
  std::string error;
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(file), &error));
  EXPECT_EQ(d.media_info().duration, int64_t{40} * 1152 * 1000000 / 44100);
  EXPECT_TRUE(d.Seek(700000));
  EncodedPacket packet;
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.buffer->data()[100], 20);
  EXPECT_EQ(packet.pts, int64_t{20} * 1152 * 1000000 / 44100);
  EXPECT_TRUE(d.Seek(0));
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 0);
  EXPECT_EQ(packet.buffer->data()[100], 0);
}
TEST(Audio, AdtsSparseIndexAndMalformedLength) {
  Bytes frame = {0xff, 0xf1, 0x50, 0x80, 1, 0x7f, 0xfc, 0};
  // 8-byte mono AAC-LC frame at 44.1 kHz.
  frame[3] = 0x40;
  frame[4] = 1;
  frame[5] = 0x1f;
  Bytes file;
  for (int i = 0; i < 100; ++i)
    Append(&file, frame);
  AdtsDemuxer d;
  std::string error;
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(file), &error));
  EXPECT_TRUE(d.Seek(1000000));
  EncodedPacket p;
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_TRUE(p.pts <= 1000000);
  EXPECT_TRUE(p.pts + p.duration > 1000000);
  file.pop_back();
  AdtsDemuxer invalid;
  EXPECT_TRUE(!invalid.Open(CreateMemoryDataSource(file), &error));
}
TEST(Audio, OggCrcGranuleAndRestart) {
  OggDemuxer d;
  std::string error;
  Bytes file = OpusOgg();
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(file), &error));
  EXPECT_EQ(d.media_info().duration, 20000);
  EncodedPacket p;
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 0);
  EXPECT_EQ(p.duration, 20000);
  EXPECT_TRUE(d.Seek(0));
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 0);
  file[20] ^= 1;
  OggDemuxer crc;
  EXPECT_TRUE(!crc.Open(CreateMemoryDataSource(file), &error));
  OggDemuxer overflow;
  EXPECT_TRUE(
      !overflow.Open(CreateMemoryDataSource(OpusOgg(INT64_MAX)), &error));
}
TEST(Audio, OggContinuedPacketAndMultiPacketGranuleSeek) {
  const Bytes head = {'O', 'p', 'u',  's',  'H', 'e', 'a', 'd', 1, 1,
                      0,   0,   0x80, 0xbb, 0,   0,   0,   0,   0};
  Bytes file = Page(0, 2, 0, {19}, head);
  Append(&file, Page(1, 0, 0, {8}, {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'}));
  Bytes spanning(300, 0x55);
  spanning[0] = 0xf8;  // One 20 ms Opus frame.
  spanning.back() = 0xa5;
  Append(&file, Page(2, 0, UINT64_MAX, {255},
                     Bytes(spanning.begin(), spanning.begin() + 255)));
  Bytes continued(spanning.begin() + 255, spanning.end());
  Append(&continued, {0xf8, 17, 0xf8, 18});
  Append(&file, Page(3, 1, 2880, {45, 2, 2}, continued));
  Append(&file, Page(4, 0, 3840, {2}, {0xf8, 19}));
  Append(&file, Page(5, 0, UINT64_MAX, {255},
                     Bytes(spanning.begin(), spanning.begin() + 255)));
  continued.assign(spanning.begin() + 255, spanning.end());
  Append(&continued, {0xf8, 20, 0xf8, 21});
  Append(&file, Page(6, 1, 6720, {45, 2, 2}, continued));
  Bytes tail;
  for (uint8_t marker = 22; marker < 29; ++marker)
    Append(&tail, {0xf8, marker});
  Append(&file, Page(7, 4, 13440, Bytes(7, 2), tail));
  OggDemuxer d;
  std::string error;
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(file), &error));
  EXPECT_EQ(d.media_info().duration, 280000);
  EncodedPacket packet;
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.size(), size_t{300});
  EXPECT_TRUE(memcmp(packet.buffer->data(), spanning.data(), spanning.size()) ==
              0);
  EXPECT_EQ(packet.pts, 0);
  EXPECT_EQ(packet.duration, 20000);
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 20000);
  EXPECT_EQ(packet.buffer->data()[1], 17);
  // The 100 ms preroll chooses page 6. Its first packet is incomplete
  // without page 5, so discard that fragment and anchor the other two
  // complete packets backwards from the page's granule of 140 ms.
  EXPECT_TRUE(d.Seek(240000));
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.size(), size_t{2});
  EXPECT_EQ(packet.pts, 100000);
  EXPECT_EQ(packet.buffer->data()[1], 20);
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 120000);
  EXPECT_EQ(packet.buffer->data()[1], 21);
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.pts, 140000);
  EXPECT_EQ(packet.buffer->data()[1], 22);
  EXPECT_TRUE(d.Seek(0));
  EXPECT_TRUE(d.Read(&packet) == DemuxerReadResult::kOk);
  EXPECT_EQ(packet.size(), size_t{300});
  EXPECT_EQ(packet.pts, 0);
}
Bytes FlacFrame(unsigned number) {
  Bytes frame = {0xff, 0xf8, 0x60, 0x08, static_cast<uint8_t>(number), 15};
  uint8_t crc = 0;
  for (uint8_t byte : frame) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit)
      crc = static_cast<uint8_t>((crc << 1) ^ ((crc & 0x80) ? 7 : 0));
  }
  frame.push_back(crc);
  frame.insert(frame.end(), {0, 0, 0, 0});
  return frame;
}
Bytes FlacFile() {
  Bytes info(34, 0);
  info[1] = 16;
  info[3] = 16;
  const uint64_t packed = (uint64_t(48000) << 44) | (uint64_t(15) << 36) | 48;
  for (int i = 0; i < 8; ++i)
    info[10 + i] = static_cast<uint8_t>(packed >> (56 - i * 8));
  Bytes file = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
  Append(&file, info);
  file.insert(file.end(), {0x83, 0, 0, 18});
  // Sample 16 is frame 1, eleven bytes after the first frame.
  for (uint64_t value : {uint64_t(16), uint64_t(11)})
    for (int i = 7; i >= 0; --i)
      file.push_back(static_cast<uint8_t>(value >> (8 * i)));
  file.insert(file.end(), {0, 16});
  for (unsigned i = 0; i < 3; ++i)
    Append(&file, FlacFrame(i));
  return file;
}
TEST(Audio, FlacStreamInfoSeekTableAndFrameTimes) {
  FlacDemuxer d;
  std::string error;
  EXPECT_TRUE(d.Open(CreateMemoryDataSource(FlacFile()), &error));
  EXPECT_EQ(d.media_info().duration, 1000);
  EncodedPacket p;
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 0);
  EXPECT_EQ(p.duration, 333);
  EXPECT_TRUE(d.Seek(700));
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 666);
  EXPECT_TRUE(d.Seek(0));
  EXPECT_TRUE(d.Read(&p) == DemuxerReadResult::kOk);
  EXPECT_EQ(p.pts, 0);
  Bytes bad = FlacFile();
  bad[7] = 33;
  FlacDemuxer invalid;
  EXPECT_TRUE(!invalid.Open(CreateMemoryDataSource(bad), &error));
  bad = FlacFile();
  bad[8 + 10] = 0;
  bad[8 + 11] = 0;
  bad[8 + 12] = 0;
  FlacDemuxer rate;
  EXPECT_TRUE(!rate.Open(CreateMemoryDataSource(bad), &error));
  bad = FlacFile();
  bad[64 + 6] ^= 1;
  FlacDemuxer crc;
  EXPECT_TRUE(!crc.Open(CreateMemoryDataSource(bad), &error));
}
class ShortUnknownSource : public DataSource {
 public:
  explicit ShortUnknownSource(Bytes bytes) : bytes_(std::move(bytes)) {}
  int64_t Read(int64_t offset, uint8_t* out, size_t size) override {
    if (offset < 0) return -1;
    if (static_cast<uint64_t>(offset) >= bytes_.size()) return 0;
    const size_t count = std::min(
        {size, size_t(3), bytes_.size() - static_cast<size_t>(offset)});
    memcpy(out, bytes_.data() + offset, count);
    return static_cast<int64_t>(count);
  }
  int64_t size() const override { return -1; }
  bool seekable() const override { return false; }
  std::string error() const override { return {}; }

 private:
  Bytes bytes_;
};
template <class D>
void ShortUnknown(Bytes bytes, int expected) {
  D d;
  std::string error;
  EXPECT_TRUE(
      d.Open(std::make_unique<ShortUnknownSource>(std::move(bytes)), &error));
  EXPECT_TRUE(!d.media_info().seekable);
  EncodedPacket p;
  int packets = 0;
  DemuxerReadResult result;
  while ((result = d.Read(&p)) == DemuxerReadResult::kOk && packets < 100)
    ++packets;
  EXPECT_EQ(packets, expected);
  EXPECT_TRUE(result == DemuxerReadResult::kEndOfStream);
}
TEST(Audio, ShortReadsWithoutContentLengthReachEof) {
  ShortUnknown<WavDemuxer>(Wave(1, 16, 1, Bytes(200)), 1);
  ShortUnknown<FlacDemuxer>(FlacFile(), 3);
  ShortUnknown<OggDemuxer>(OpusOgg(), 1);
  Bytes adts = {0xff, 0xf1, 0x50, 0x40, 1, 0x1f, 0xfc, 0};
  ShortUnknown<AdtsDemuxer>(adts, 1);
  Bytes mp3(417, 0);
  mp3[0] = 0xff;
  mp3[1] = 0xfb;
  mp3[2] = 0x90;
  ShortUnknown<Mp3Demuxer>(mp3, 1);
}
TEST(Audio, PcmConvertsIntegerAndRejectsNonfiniteFloat) {
  AudioConfig config = {AudioCodec::kPCM, "pcm_s24le", 48000, 1, {24, 0, 0}};
  PcmAudioDecoder d;
  std::string error;
  EXPECT_TRUE(d.Initialize(config, {48000, 2}, &error));
  EncodedPacket p;
  p.pts = 123;
  p.buffer = PacketBuffer::Copy(Bytes{0, 0, 0x80, 0xff, 0xff, 0x7f}.data(), 6);
  std::shared_ptr<AudioBuffer> output;
  EXPECT_TRUE(d.Decode(p, [&](auto b) { output = std::move(b); }));
  EXPECT_TRUE(output);
  EXPECT_EQ(output->pts, 123);
  EXPECT_EQ(output->frames(), 2);
  EXPECT_EQ(output->samples[0], -1.0f);
  EXPECT_TRUE(output->samples[2] > 0.99f);
  config.extradata = {32, 1, 0};
  PcmAudioDecoder f;
  EXPECT_TRUE(f.Initialize(config, {48000, 1}, &error));
  const uint32_t nan = 0x7fc00000;
  p.buffer = PacketBuffer::Copy(reinterpret_cast<const uint8_t*>(&nan), 4);
  EXPECT_TRUE(f.Decode(p, [&](auto b) { output = std::move(b); }));
  EXPECT_EQ(output->samples[0], 0.0f);
}
}  // namespace
}  // namespace lychee
// NOLINTEND(bugprone-throwing-static-initialization)
