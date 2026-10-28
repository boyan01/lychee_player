#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <vector>

#include "backends/mp2t/elementary_stream.h"
#include "backends/mp2t/ts_demuxer.h"
#include "backends/mp2t/ts_parser.h"
#include "backends/mp2t/ts_reader.h"
#include "test.h"

namespace lychee {
namespace {

using Bytes = std::vector<uint8_t>;

uint32_t Crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < size; ++i) {
    crc ^= static_cast<uint32_t>(data[i]) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      crc = crc & 0x80000000 ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
  }
  return crc;
}

// Writes transport stream packets, the way a muxer would.
class TsWriter {
 public:
  Bytes out;

  // One packet; |payload| fits (at most 184 bytes, 182 with |random_access|).
  void Packet(int pid,
              bool start,
              const uint8_t* payload,
              size_t size,
              bool random_access = false) {
    const size_t at = out.size();
    out.resize(at + 188, 0xff);
    uint8_t* p = &out[at];
    p[0] = 0x47;
    p[1] = static_cast<uint8_t>((start ? 0x40 : 0) | (pid >> 8));
    p[2] = static_cast<uint8_t>(pid);
    const bool adaptation = size < 184 || random_access;
    p[3] =
        static_cast<uint8_t>((adaptation ? 0x30 : 0x10) | (cc_[pid]++ & 0xf));
    size_t header = 4;
    if (adaptation) {
      const size_t length = 183 - size;
      p[4] = static_cast<uint8_t>(length);
      if (length > 0) {
        p[5] = random_access ? 0x40 : 0;
      }
      header = 5 + length;
    }
    memcpy(p + header, payload, size);
  }

  void Section(int pid, Bytes section) {
    const uint32_t crc = Crc32(section.data(), section.size());
    for (int shift = 24; shift >= 0; shift -= 8) {
      section.push_back(static_cast<uint8_t>(crc >> shift));
    }
    section.insert(section.begin(), 0);  // pointer_field
    Packet(pid, true, section.data(), section.size());
  }

  void Pat(int pmt_pid) {
    Bytes s = {0x00,
               0xb0,
               0,
               0x00,
               0x01,
               0xc1,
               0x00,
               0x00,
               0x00,
               0x01,
               static_cast<uint8_t>(0xe0 | (pmt_pid >> 8)),
               static_cast<uint8_t>(pmt_pid)};
    s[2] = static_cast<uint8_t>(s.size() - 3 + 4);
    Section(0, s);
  }

  struct Entry {
    int type;
    int pid;
    Bytes descriptors;
  };
  void Pmt(int pmt_pid,
           int version,
           const std::vector<Entry>& entries,
           bool corrupt = false) {
    Bytes s = {0x02, 0xb0, 0,
               0x00, 0x01, static_cast<uint8_t>(0xc1 | (version << 1)),
               0x00, 0x00, 0xe1,
               0x00, 0xf0, 0x00};
    for (const Entry& e : entries) {
      s.push_back(static_cast<uint8_t>(e.type));
      s.push_back(static_cast<uint8_t>(0xe0 | (e.pid >> 8)));
      s.push_back(static_cast<uint8_t>(e.pid));
      s.push_back(static_cast<uint8_t>(0xf0 | (e.descriptors.size() >> 8)));
      s.push_back(static_cast<uint8_t>(e.descriptors.size()));
      s.insert(s.end(), e.descriptors.begin(), e.descriptors.end());
    }
    s[2] = static_cast<uint8_t>(s.size() - 3 + 4);
    Section(pmt_pid, s);
    if (corrupt) {
      // The section ends the packet, before its CRC: no longer matches.
      out[out.size() - 5] ^= 1;
    }
  }

  static void Timestamp(int prefix, int64_t ts, Bytes* out) {
    out->push_back(
        static_cast<uint8_t>((prefix << 4) | ((ts >> 29) & 0x0e) | 1));
    out->push_back(static_cast<uint8_t>(ts >> 22));
    out->push_back(static_cast<uint8_t>(((ts >> 14) & 0xfe) | 1));
    out->push_back(static_cast<uint8_t>(ts >> 7));
    out->push_back(static_cast<uint8_t>(((ts << 1) & 0xfe) | 1));
  }

  // A PES packet; |dts| -1 for none. Returns the TS packets' offsets.
  std::vector<size_t> Pes(int pid,
                          int stream_id,
                          int64_t pts,
                          int64_t dts,
                          const Bytes& data,
                          bool random_access = false,
                          bool known_length = false) {
    Bytes pes = {0,
                 0,
                 1,
                 static_cast<uint8_t>(stream_id),
                 0,
                 0,
                 0x80,
                 static_cast<uint8_t>(dts >= 0 ? 0xc0 : 0x80),
                 static_cast<uint8_t>(dts >= 0 ? 10 : 5)};
    Timestamp(dts >= 0 ? 3 : 2, pts, &pes);
    if (dts >= 0) {
      Timestamp(1, dts, &pes);
    }
    if (known_length) {
      const size_t length = pes.size() - 6 + data.size();
      pes[4] = static_cast<uint8_t>(length >> 8);
      pes[5] = static_cast<uint8_t>(length);
    }
    pes.insert(pes.end(), data.begin(), data.end());
    std::vector<size_t> offsets;
    size_t i = 0;
    bool first = true;
    while (i < pes.size()) {
      const size_t room = first && random_access ? 182 : 184;
      const size_t size = std::min(room, pes.size() - i);
      offsets.push_back(out.size());
      Packet(pid, first, pes.data() + i, size, first && random_access);
      i += size;
      first = false;
    }
    return offsets;
  }

 private:
  std::map<int, int> cc_;
};

struct Collector : TsParser::Client {
  struct Pes {
    int pid;
    Bytes data;
    int64_t pts;
    int64_t dts;
    bool random_access;
  };
  std::vector<std::vector<TsStreamEntry>> programs;
  std::vector<Pes> pes;
  void OnProgram(const std::vector<TsStreamEntry>& streams) override {
    programs.push_back(streams);
  }
  void OnPes(const PesPacket& p) override {
    pes.push_back(
        {p.pid, Bytes(p.data, p.data + p.size), p.pts, p.dts, p.random_access});
  }
};

void Parse(TsParser* parser, const Bytes& data) {
  for (size_t i = 0; i + 188 <= data.size(); i += 188) {
    parser->Parse(&data[i], static_cast<int64_t>(i));
  }
}

Bytes Pattern(size_t size, uint8_t seed) {
  Bytes data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>(seed + i * 7);
  }
  return data;
}

// 1920x1080 (1088 coded, cropped), 640x360 (368 coded), and 720x576
// with a 64:45 sample aspect ratio (extended SAR), from x264; HEVC
// 1920x1080 (1088 coded, conformance window) from x265.
const Bytes kSps1080 = {0x67, 0x64, 0x00, 0x28, 0xac, 0xd9, 0x40, 0x78, 0x02,
                        0x27, 0xe5, 0xc0, 0x44, 0x00, 0x00, 0x03, 0x00, 0x04,
                        0x00, 0x00, 0x03, 0x00, 0xf0, 0x3c, 0x60, 0xc6, 0x58};
const Bytes kSps360 = {0x67, 0x64, 0x00, 0x1e, 0xac, 0xd9, 0x40, 0xa0, 0x2f,
                       0xf9, 0x70, 0x11, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
                       0x00, 0x03, 0x00, 0x3c, 0x0f, 0x16, 0x2d, 0x96};
const Bytes kSpsSar = {0x67, 0x64, 0x00, 0x1e, 0xac, 0xd9, 0x40, 0xb4, 0x12,
                       0x6f, 0xfc, 0x01, 0x00, 0x00, 0xb5, 0xa8, 0x10, 0x10,
                       0x0a, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00, 0x03,
                       0x00, 0x64, 0x1e, 0x2c, 0x5b, 0x2c};
const Bytes kHevcSps1080 = {
    0x42, 0x01, 0x01, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x03, 0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x11, 0x07,
    0xca, 0xd9, 0x65, 0x65, 0x4a, 0x4c, 0x2f, 0x01, 0x68, 0x08, 0x00, 0x00,
    0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xc0, 0x40};

// An access unit: start code + |sps| (if any) + a slice of |slice_type|.
Bytes AccessUnit(const Bytes& sps, uint8_t slice_header, size_t slice_size) {
  Bytes au;
  if (!sps.empty()) {
    au.insert(au.end(), {0, 0, 0, 1});
    au.insert(au.end(), sps.begin(), sps.end());
  }
  au.insert(au.end(), {0, 0, 1, slice_header});
  if (slice_header == 0x26 || slice_header == 0x02) {
    au.push_back(0x01);  // HEVC: second NAL header byte.
  }
  Bytes slice = Pattern(slice_size, 0x11);
  for (uint8_t& b : slice) {
    b |= 0x80;  // No start code inside.
  }
  au.insert(au.end(), slice.begin(), slice.end());
  return au;
}

Bytes Adts(int rate_index, size_t payload) {
  const size_t length = 7 + payload;
  Bytes frame = {0xff,
                 0xf1,
                 static_cast<uint8_t>((1 << 6) | (rate_index << 2)),
                 static_cast<uint8_t>((2 << 6) | (length >> 11)),
                 static_cast<uint8_t>(length >> 3),
                 static_cast<uint8_t>(((length & 7) << 5) | 0x1f),
                 0xfc};
  Bytes body = Pattern(payload, 0x42);
  frame.insert(frame.end(), body.begin(), body.end());
  return frame;
}

class MemorySource : public DataSource {
 public:
  explicit MemorySource(Bytes data) : data_(std::move(data)) {}
  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (offset >= static_cast<int64_t>(data_.size())) {
      return 0;
    }
    size = std::min(size, data_.size() - static_cast<size_t>(offset));
    memcpy(data, data_.data() + offset, size);
    return static_cast<int64_t>(size);
  }
  int64_t size() const override { return static_cast<int64_t>(data_.size()); }
  std::string error() const override { return ""; }

 private:
  Bytes data_;
};

}  // namespace

TEST(Mp2t, ParsesProgramAndPes) {
  TsWriter w;
  w.Pat(0x1000);
  w.Pmt(0x1000, 0,
        {{0x1b, 0x100, {}},
         {0x0f, 0x101, {0x0a, 0x04, 'e', 'n', 'g', 0x00}},
         {0x15, 0x102, {}}});
  Bytes video = Pattern(500, 1);
  w.Pes(0x100, 0xe0, 9000, 6000, video, true);
  Bytes audio = Pattern(100, 2);
  w.Pes(0x101, 0xc0, 12000, -1, audio, false, true);
  Collector c;
  TsParser parser(&c);
  Parse(&parser, w.out);
  EXPECT_EQ(c.programs.size(), 1u);
  EXPECT_EQ(c.programs[0].size(), 3u);
  EXPECT_EQ(c.programs[0][1].language, std::string("eng"));
  // Audio has a length: out at once. Video waits for the end.
  EXPECT_EQ(c.pes.size(), 1u);
  parser.Flush();
  EXPECT_EQ(c.pes.size(), 2u);
  EXPECT_EQ(c.pes[0].pid, 0x101);
  EXPECT_TRUE(c.pes[0].data == audio);
  EXPECT_EQ(c.pes[0].pts, 12000);
  EXPECT_EQ(c.pes[0].dts, -1);
  EXPECT_EQ(c.pes[1].pid, 0x100);
  EXPECT_TRUE(c.pes[1].data == video);
  EXPECT_EQ(c.pes[1].pts, 9000);
  EXPECT_EQ(c.pes[1].dts, 6000);
  EXPECT_TRUE(c.pes[1].random_access);
}

TEST(Mp2t, ContinuityErrorsAndDuplicates) {
  TsWriter w;
  w.Pat(0x1000);
  w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}});
  const size_t start = w.out.size();
  std::vector<size_t> packets = w.Pes(0x100, 0xe0, 9000, -1, Pattern(700, 3));
  w.Pes(0x100, 0xe0, 12000, -1, Pattern(10, 4));
  // The second packet of the first PES is lost; its third is sent twice.
  Bytes data(w.out.begin(), w.out.begin() + static_cast<ptrdiff_t>(start));
  for (size_t i = 0; i < packets.size(); ++i) {
    if (i == 1) {
      continue;
    }
    data.insert(data.end(), w.out.begin() + static_cast<ptrdiff_t>(packets[i]),
                w.out.begin() + static_cast<ptrdiff_t>(packets[i] + 188));
    if (i == 2) {
      data.insert(data.end(),
                  w.out.begin() + static_cast<ptrdiff_t>(packets[i]),
                  w.out.begin() + static_cast<ptrdiff_t>(packets[i] + 188));
    }
  }
  data.insert(data.end(),
              w.out.begin() + static_cast<ptrdiff_t>(packets.back() + 188),
              w.out.end());
  Collector c;
  TsParser parser(&c);
  Parse(&parser, data);
  parser.Flush();
  EXPECT_EQ(c.pes.size(), 2u);
  // Damaged but handed out: 184 bytes short, the duplicate not doubled.
  EXPECT_EQ(c.pes[0].data.size(), 700u - 184u);
  EXPECT_EQ(c.pes[1].pts, 12000);
}

TEST(Mp2t, PmtVersionsAndCrc) {
  TsWriter w;
  w.Pat(0x1000);
  w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}});
  w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}});        // Repeated: no new program.
  w.Pmt(0x1000, 1, {{0x1b, 0x200, {}}}, true);  // Bad CRC: ignored.
  w.Pmt(0x1000, 1, {{0x1b, 0x200, {}}, {0x0f, 0x201, {}}});
  w.Pes(0x100, 0xe0, 1000, -1, Pattern(10, 5));  // Gone: not handed out.
  w.Pes(0x200, 0xe0, 2000, -1, Pattern(10, 6));
  Collector c;
  TsParser parser(&c);
  Parse(&parser, w.out);
  parser.Flush();
  EXPECT_EQ(c.programs.size(), 2u);
  EXPECT_EQ(c.programs[1].size(), 2u);
  EXPECT_EQ(c.programs[1][0].pid, 0x200);
  EXPECT_EQ(c.pes.size(), 1u);
  EXPECT_EQ(c.pes[0].pid, 0x200);
}

TEST(Mp2t, RandomPacketsAreHarmless) {
  TsWriter w;
  w.Pat(0x1000);
  w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}, {0x0f, 0x101, {}}});
  std::mt19937 random(7);
  const int pids[] = {0, 0x1000, 0x100, 0x101};
  for (int i = 0; i < 20000; ++i) {
    Bytes packet(188);
    for (uint8_t& b : packet) {
      b = static_cast<uint8_t>(random());
    }
    packet[0] = 0x47;
    const int pid = pids[random() % 4];
    packet[1] = static_cast<uint8_t>((packet[1] & 0xe0) | (pid >> 8));
    packet[2] = static_cast<uint8_t>(pid);
    if (i % 3 == 0) {
      packet[1] &= 0x7f;  // No transport error...
      packet[1] |= 0x40;  // ...and a unit start,
      packet[3] = static_cast<uint8_t>(0x10 | (packet[3] & 0xf));
      packet[4] = packet[5] = 0;  // often a PES start code
      packet[6] = 1;
    }
    w.out.insert(w.out.end(), packet.begin(), packet.end());
  }
  Collector c;
  TsParser parser(&c);
  Parse(&parser, w.out);
  parser.Flush();
  EXPECT_TRUE(!c.programs.empty());
  // The same bytes through the reader, which also parses what it finds.
  MemorySource source(w.out);
  TsReader reader(&source);
  reader.SetRange(0, -1);
  std::string error;
  EXPECT_TRUE(reader.Probe(1 << 20, &error));
  EncodedPacket packet;
  int64_t offset;
  int count = 0;
  while (reader.Read(&packet, &offset) == DemuxerReadResult::kOk) {
    EXPECT_TRUE(packet.stream_index >= 0 && packet.stream_index < 2);
    ++count;
  }
  EXPECT_TRUE(count > 0);
}

TEST(Mp2t, ParsesSps) {
  AccessUnitInfo info;
  Bytes au = AccessUnit(kSps1080, 0x65, 50);
  EXPECT_TRUE(ParseAccessUnit(au.data(), au.size(), false, &info));
  EXPECT_TRUE(info.has_sps);
  EXPECT_TRUE(info.keyframe);
  EXPECT_EQ(info.width, 1920);
  EXPECT_EQ(info.height, 1080);
  EXPECT_EQ(info.sar_num, 1);

  au = AccessUnit(kSps360, 0x41, 50);
  EXPECT_TRUE(ParseAccessUnit(au.data(), au.size(), false, &info));
  EXPECT_TRUE(!info.keyframe);
  EXPECT_EQ(info.width, 640);
  EXPECT_EQ(info.height, 360);

  au = AccessUnit(kSpsSar, 0x65, 50);
  EXPECT_TRUE(ParseAccessUnit(au.data(), au.size(), false, &info));
  EXPECT_EQ(info.width, 720);
  EXPECT_EQ(info.height, 576);
  EXPECT_EQ(info.sar_num, 64);
  EXPECT_EQ(info.sar_den, 45);

  // HEVC: IDR_W_RADL (type 19) is a keyframe, TRAIL_R (type 1) is not.
  au = AccessUnit(kHevcSps1080, 0x26, 50);
  EXPECT_TRUE(ParseAccessUnit(au.data(), au.size(), true, &info));
  EXPECT_TRUE(info.has_sps);
  EXPECT_TRUE(info.keyframe);
  EXPECT_EQ(info.width, 1920);
  EXPECT_EQ(info.height, 1080);
  au = AccessUnit({}, 0x02, 50);
  EXPECT_TRUE(ParseAccessUnit(au.data(), au.size(), true, &info));
  EXPECT_TRUE(!info.keyframe && !info.has_sps);

  // Truncated parameter sets are not believed.
  for (size_t cut = 1; cut < kSps1080.size() - 4; ++cut) {
    Bytes partial(kSps1080.begin(),
                  kSps1080.begin() + static_cast<ptrdiff_t>(cut));
    Bytes truncated = {0, 0, 1};
    truncated.insert(truncated.end(), partial.begin(), partial.end());
    ParseAccessUnit(truncated.data(), truncated.size(), false, &info);
    EXPECT_TRUE(!info.has_sps || info.height <= 1088);
  }
}

TEST(Mp2t, ParsesAdtsHeader) {
  Bytes frame = Adts(3, 100);
  AdtsHeader header;
  EXPECT_TRUE(ParseAdtsHeader(frame.data(), frame.size(), &header));
  EXPECT_EQ(header.sample_rate, 48000);
  EXPECT_EQ(header.channels, 2);
  EXPECT_EQ(header.object_type, 2);
  EXPECT_EQ(header.frame_length, 107u);
  EXPECT_EQ(header.header_size, 7u);
  EXPECT_EQ(header.samples, 1024);
  frame[2] |= 0x3c;  // Sampling frequency index 15: reserved.
  EXPECT_TRUE(!ParseAdtsHeader(frame.data(), frame.size(), &header));
}

TEST(TsReader, UnwrapsTimestampsAndSplitsAdts) {
  TsWriter w;
  w.Pat(0x1000);
  w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}, {0x0f, 0x101, {}}});
  const int64_t wrap = int64_t{1} << 33;
  // Four frames at 30 fps across the 33-bit wrap.
  for (int i = 0; i < 4; ++i) {
    const int64_t pts = (wrap - 6000 + i * 3000) % wrap;
    w.Pes(0x100, 0xe0, pts, -1,
          AccessUnit(i == 0 ? kSps360 : Bytes(), i == 0 ? 0x65 : 0x41, 300),
          i == 0);
  }
  // Three ADTS frames at 48 kHz, the third split over two PES packets.
  Bytes frames;
  for (int i = 0; i < 3; ++i) {
    Bytes frame = Adts(3, 50 + i);
    frames.insert(frames.end(), frame.begin(), frame.end());
  }
  const size_t split = frames.size() - 20;
  w.Pes(0x101, 0xc0, wrap - 3000, -1,
        Bytes(frames.begin(), frames.begin() + static_cast<ptrdiff_t>(split)),
        false, true);
  w.Pes(0x101, 0xc0, 1000, -1,
        Bytes(frames.begin() + static_cast<ptrdiff_t>(split), frames.end()),
        false, true);

  MemorySource source(w.out);
  EXPECT_TRUE(TsReader::Detect(w.out.data(), w.out.size()));
  TsReader reader(&source);
  reader.SetRange(0, -1);
  std::string error;
  EXPECT_TRUE(reader.Probe(1 << 20, &error));
  EXPECT_EQ(reader.streams().size(), 2u);
  EXPECT_EQ(reader.streams()[0].info.video.width, 640);
  EXPECT_EQ(reader.streams()[0].info.video.height, 360);
  EXPECT_EQ(reader.streams()[1].info.audio.sample_rate, 48000);
  EXPECT_EQ(reader.streams()[1].info.audio.channels, 2);

  std::vector<EncodedPacket> video;
  std::vector<EncodedPacket> audio;
  EncodedPacket packet;
  int64_t offset;
  while (reader.Read(&packet, &offset) == DemuxerReadResult::kOk) {
    (packet.stream_index == 0 ? video : audio).push_back(packet);
  }
  EXPECT_EQ(video.size(), 4u);
  EXPECT_EQ(audio.size(), 3u);
  for (size_t i = 0; i < video.size(); ++i) {
    const int64_t ticks = wrap - 6000 + static_cast<int64_t>(i) * 3000;
    EXPECT_EQ(video[i].pts, TsReader::TicksToMicros(ticks));
    // The distance to the next frame; the last one repeats it.
    EXPECT_NEAR(video[i].duration, 33333, 1);
    EXPECT_EQ(video[i].keyframe, i == 0);
  }
  const int64_t audio_start = TsReader::TicksToMicros(wrap - 3000);
  EXPECT_EQ(audio[0].pts, audio_start);
  EXPECT_EQ(audio[1].pts, audio_start + 21333);
  // The split frame keeps the timeline it started on.
  EXPECT_EQ(audio[2].pts, audio_start + 42666);
  EXPECT_EQ(audio[2].duration, 21334);
  EXPECT_EQ(audio[2].size(), 7u + 52u);
}

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(Mp2t, ParsesMp3Header) {
  AdtsHeader header;
  // MPEG-1 Layer III, 128 kbit/s, 44.1 kHz, joint stereo.
  const Bytes mpeg1 = {0xff, 0xfb, 0x90, 0x64};
  EXPECT_TRUE(ParseMp3Header(mpeg1.data(), mpeg1.size(), &header));
  EXPECT_EQ(header.sample_rate, 44100);
  EXPECT_EQ(header.channels, 2);
  EXPECT_EQ(header.samples, 1152);
  EXPECT_EQ(header.frame_length, 417u);
  // MPEG-2, 32 kbit/s, 16 kHz, padded, mono: half the samples.
  const Bytes mpeg2 = {0xff, 0xf3, 0x4a, 0xc4};
  EXPECT_TRUE(ParseMp3Header(mpeg2.data(), mpeg2.size(), &header));
  EXPECT_EQ(header.sample_rate, 16000);
  EXPECT_EQ(header.channels, 1);
  EXPECT_EQ(header.samples, 576);
  EXPECT_EQ(header.frame_length, 145u);
  // Layer II, a free bitrate, a reserved rate: not taken.
  for (const Bytes& bad :
       {Bytes{0xff, 0xfd, 0x90, 0x64}, Bytes{0xff, 0xfb, 0x00, 0x64},
        Bytes{0xff, 0xfb, 0x9c, 0x64}}) {
    EXPECT_TRUE(!ParseMp3Header(bad.data(), bad.size(), &header));
  }
}

TEST(Mp2t, ParsesAc3Header) {
  // FFmpeg's: AC-3 5.1 at 48 kHz (acmod 7, LFE), E-AC-3 stereo at 32 kHz.
  const Bytes ac3 = {0x0b, 0x77, 0x2b, 0x9a, 0x1e, 0x40, 0xeb, 0xf8};
  const Bytes eac3 = {0x0b, 0x77, 0x02, 0x3f, 0xb4, 0x87, 0xc0, 0x00};
  AdtsHeader header;
  EXPECT_TRUE(ParseAc3Header(ac3.data(), ac3.size(), &header));
  EXPECT_EQ(header.sample_rate, 48000);
  EXPECT_EQ(header.channels, 6);
  EXPECT_TRUE(ParseAc3Header(eac3.data(), eac3.size(), &header));
  EXPECT_EQ(header.sample_rate, 32000);
  EXPECT_EQ(header.channels, 2);
  EXPECT_TRUE(!ParseAc3Header(ac3.data() + 1, ac3.size() - 1, &header));
}

// Video at 30 fps in runs of |frames| that each start over at 10 s, a
// keyframe every second, the PAT and PMT before each.
Bytes RestartingStream(int runs, int frames, size_t frame_size) {
  TsWriter w;
  for (int run = 0; run < runs; ++run) {
    for (int i = 0; i < frames; ++i) {
      const bool key = i % 30 == 0;
      if (key) {
        w.Pat(0x1000);
        w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}});
      }
      w.Pes(0x100, 0xe0, 900000 + int64_t{i} * 3000, -1,
            AccessUnit(key ? kSps360 : Bytes(), key ? 0x65 : 0x41, frame_size),
            key);
    }
  }
  return w.out;
}

TEST(TsReader, JoinsRunsWhereTimestampsStartOver) {
  MemorySource source(RestartingStream(3, 45, 300));
  TsTimeline timeline(1);
  TsReader reader(&source);
  reader.JoinRuns(&timeline);
  reader.SetRange(0, -1);
  std::string error;
  EXPECT_TRUE(reader.Probe(1 << 20, &error));
  EncodedPacket packet;
  int64_t offset;
  int64_t expected = 10 * kMicrosPerSecond;
  int count = 0;
  while (reader.Read(&packet, &offset) == DemuxerReadResult::kOk) {
    EXPECT_NEAR(packet.pts, expected, 1);
    EXPECT_NEAR(packet.duration, 33333, 1);
    expected = packet.pts + 33333;
    ++count;
  }
  EXPECT_EQ(count, 135);
  EXPECT_EQ(timeline.size(), 3u);
  // Read again from the second run: the timeline places it.
  reader.Restart(timeline[1].offset, 11500000);
  EXPECT_TRUE(reader.Read(&packet, &offset) == DemuxerReadResult::kOk);
  EXPECT_NEAR(packet.pts, 11500000, 1);
}

TEST(TsDemuxer, FindsRunsWithoutReadingEverything) {
  // 3.6 MB: the end the duration reads is in the last run only.
  TsDemuxer demuxer;
  std::string error;
  EXPECT_TRUE(demuxer.Open(
      std::make_unique<MemorySource>(RestartingStream(3, 150, 8000)), &error));
  const int64_t start = 10 * kMicrosPerSecond;
  EXPECT_EQ(demuxer.media_info().start_time, start);
  EXPECT_NEAR(demuxer.media_info().duration, int64_t{450} * 33333, 2000);
  // Lands on the keyframe at or before, in whichever run plays then.
  for (int64_t target : {1500000, 7200000, 12500000, 14900000}) {
    EXPECT_TRUE(demuxer.Seek(start + target));
    EncodedPacket packet;
    EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
    EXPECT_TRUE(packet.keyframe);
    // Each run's shift rounds to the microsecond.
    EXPECT_NEAR(packet.pts,
                start + target / kMicrosPerSecond * kMicrosPerSecond, 3);
  }
  // And plays on across the next reset.
  EXPECT_TRUE(demuxer.Seek(start + 4500000));
  EncodedPacket packet;
  int64_t expected = start + 4000000;
  for (int i = 0; i < 60; ++i) {
    EXPECT_TRUE(demuxer.Read(&packet) == DemuxerReadResult::kOk);
    EXPECT_NEAR(packet.pts, expected, 1);
    expected = packet.pts + 33333;
  }
}

TEST(TsReader, DropsAudioOverlappingTheRunBefore) {
  // Two files joined: in each, 30 video frames from 10 s, and AAC at
  // 48 kHz from two frames before the video to two frames after it. (PES
  // packets of a known length, so the reader sees the second run's video
  // first, which places the run.)
  TsWriter w;
  for (int run = 0; run < 2; ++run) {
    w.Pat(0x1000);
    w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}, {0x0f, 0x101, {}}});
    int64_t audio = 900000 - 3840;
    for (int i = 0; i < 30; ++i) {
      const int64_t pts = 900000 + int64_t{i} * 3000;
      w.Pes(0x100, 0xe0, pts, -1,
            AccessUnit(i == 0 ? kSps360 : Bytes(), i == 0 ? 0x65 : 0x41, 300),
            i == 0, true);
      for (; audio < pts + 3000; audio += 1920) {
        w.Pes(0x101, 0xc0, audio, -1, Adts(3, 50), false, true);
      }
    }
    for (; audio < 990000 + 3840; audio += 1920) {
      w.Pes(0x101, 0xc0, audio, -1, Adts(3, 50), false, true);
    }
  }
  MemorySource source(w.out);
  TsTimeline timeline(1);
  TsReader reader(&source);
  reader.JoinRuns(&timeline);
  reader.SetRange(0, -1);
  std::string error;
  EXPECT_TRUE(reader.Probe(1 << 20, &error));
  std::vector<EncodedPacket> video;
  std::vector<EncodedPacket> audio;
  EncodedPacket packet;
  int64_t offset;
  while (reader.Read(&packet, &offset) == DemuxerReadResult::kOk) {
    (packet.stream_index == 0 ? video : audio).push_back(packet);
  }
  EXPECT_EQ(video.size(), 60u);
  EXPECT_NEAR(video[30].pts, video[29].pts + 33333, 1);
  // The second run's first four frames start more than half a frame
  // before the first run's audio ends; its later ones keep their time.
  EXPECT_EQ(audio.size(), 51u + 47u);
  for (size_t i = 1; i < audio.size(); ++i) {
    EXPECT_TRUE(audio[i].pts + audio[i].duration / 2 >=
                audio[i - 1].pts + audio[i - 1].duration);
  }
  EXPECT_NEAR(audio[51].pts, TsReader::TicksToMicros(986160 + 4 * 1920), 1);
}

TEST(TsDemuxer, RefusesStreamsItWouldSkip) {
  // H.264, and next to it what TsReader does not take.
  auto stream = [](const TsWriter::Entry& other) {
    TsWriter w;
    w.Pat(0x1000);
    w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}, other});
    for (int i = 0; i < 3; ++i) {
      w.Pes(0x100, 0xe0, 900000 + int64_t{i} * 3000, -1,
            AccessUnit(i == 0 ? kSps360 : Bytes(), i == 0 ? 0x65 : 0x41, 300),
            i == 0);
    }
    return w.out;
  };
  // MPEG-2 video, DVB subtitles (a subtitling descriptor), and MPEG
  // audio that is not Layer III.
  const Bytes subtitling = {0x59, 8, 'e', 'n', 'g', 0x10, 0, 1, 0, 1};
  const Bytes layer2 = Bytes{0xff, 0xfd, 0x90, 0x64};
  for (const TsWriter::Entry& other :
       {TsWriter::Entry{0x02, 0x101, {}},
        TsWriter::Entry{0x06, 0x101, subtitling}}) {
    TsDemuxer refusing;
    refusing.set_refuse_skipped_streams(true);
    std::string error;
    EXPECT_TRUE(
        !refusing.Open(std::make_unique<MemorySource>(stream(other)), &error));
    EXPECT_TRUE(error.find("stream type 0x0") == 0);
    // Without another demuxer to go to, the H.264 plays.
    TsDemuxer playing;
    EXPECT_TRUE(
        playing.Open(std::make_unique<MemorySource>(stream(other)), &error));
    EXPECT_EQ(playing.media_info().streams.size(), 1u);
  }
  {
    TsWriter w;
    w.Pat(0x1000);
    w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}, {0x03, 0x101, {}}});
    for (int i = 0; i < 3; ++i) {
      w.Pes(0x100, 0xe0, 900000 + int64_t{i} * 3000, -1,
            AccessUnit(i == 0 ? kSps360 : Bytes(), i == 0 ? 0x65 : 0x41, 300),
            i == 0);
      Bytes frame = layer2;
      frame.resize(400, 0x42);
      w.Pes(0x101, 0xc0, 900000 + int64_t{i} * 3000, -1, frame, false, true);
    }
    TsDemuxer refusing;
    refusing.set_refuse_skipped_streams(true);
    std::string error;
    EXPECT_TRUE(!refusing.Open(std::make_unique<MemorySource>(w.out), &error));
    EXPECT_EQ(error, "stream type 0x03 not supported");
  }
  // Data (ID3 metadata, SCTE-35 cues) is no reason to refuse.
  for (int data : {0x15, 0x86}) {
    TsDemuxer demuxer;
    demuxer.set_refuse_skipped_streams(true);
    std::string error;
    EXPECT_TRUE(demuxer.Open(
        std::make_unique<MemorySource>(stream({data, 0x101, {}})), &error));
  }
}

// NOLINTEND(bugprone-throwing-static-initialization)

TEST(TsReader, DetectsPacketSizes) {
  TsWriter w;
  w.Pat(0x1000);
  w.Pmt(0x1000, 0, {{0x1b, 0x100, {}}});
  w.Pes(0x100, 0xe0, 1000, -1, Pattern(400, 1));
  EXPECT_TRUE(TsReader::Detect(w.out.data(), w.out.size()));
  // M2TS: a four-byte timecode before every packet.
  Bytes m2ts;
  for (size_t i = 0; i < w.out.size(); i += 188) {
    m2ts.insert(m2ts.end(), {0, 0, 0, 0});
    m2ts.insert(m2ts.end(), w.out.begin() + static_cast<ptrdiff_t>(i),
                w.out.begin() + static_cast<ptrdiff_t>(i + 188));
  }
  EXPECT_TRUE(TsReader::Detect(m2ts.data(), m2ts.size()));
  MemorySource source(m2ts);
  TsReader reader(&source);
  reader.SetRange(0, -1);
  std::string error;
  EXPECT_TRUE(reader.Probe(1 << 20, &error));
  EXPECT_EQ(reader.packet_size(), 192);
  const char* mp4 =
      "\0\0\0\x20"
      "ftypisom";
  EXPECT_TRUE(!TsReader::Detect(reinterpret_cast<const uint8_t*>(mp4), 12));
}

}  // namespace lychee
