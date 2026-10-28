// Fragmented MP4 layouts FFmpeg's muxer does not write (mehd, no tfdt),
// and tx3g text (FFmpeg-free builds only).

#include "backends/mp4/mp4_demuxer.h"
#include "media/text_subtitle_decoder.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

using Bytes = std::vector<uint8_t>;

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
  box.insert(box.end(), type, type + 4);
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
