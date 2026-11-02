#include "media/pgs_subtitle_decoder.h"

#include <algorithm>
#include <string>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

using Bytes = std::vector<uint8_t>;

void Put16(Bytes* out, int value) {
  out->push_back(static_cast<uint8_t>(value >> 8));
  out->push_back(static_cast<uint8_t>(value));
}

void Segment(Bytes* out, uint8_t type, const Bytes& payload) {
  out->push_back(type);
  Put16(out, static_cast<int>(payload.size()));
  out->insert(out->end(), payload.begin(), payload.end());
}

// A composition on a 1920x1080 canvas: |state| 0x80 starts an epoch, 0
// reuses its objects. Each placement is object 0 at (x, y), cropped to
// |crop| (x, y, width, height) when it has four values.
Bytes Composition(uint8_t state,
                  int palette,
                  const std::vector<std::vector<int>>& placements) {
  Bytes pcs;
  Put16(&pcs, 1920);
  Put16(&pcs, 1080);
  pcs.push_back(0x10);
  Put16(&pcs, 1);
  pcs.push_back(state);
  pcs.push_back(0);
  pcs.push_back(static_cast<uint8_t>(palette));
  pcs.push_back(static_cast<uint8_t>(placements.size()));
  for (const auto& placement : placements) {
    Put16(&pcs, 0);
    pcs.push_back(0);
    pcs.push_back(placement.size() > 2 ? 0x80 : 0);
    Put16(&pcs, placement[0]);
    Put16(&pcs, placement[1]);
    for (size_t i = 2; i < placement.size(); ++i) {
      Put16(&pcs, placement[i]);
    }
  }
  Bytes out;
  Segment(&out, 0x16, pcs);
  return out;
}

// Palette |id|: 1 white, 2 black at half alpha.
Bytes Palette(int id) {
  Bytes pds = {
      static_cast<uint8_t>(id), 0, 1, 235, 128, 128, 255, 2, 16, 128, 128, 128};
  Bytes out;
  Segment(&out, 0x14, pds);
  return out;
}

// A 4x2 object: a row of white, white, black, black; then a row of one
// transparent pixel and three white. In ODS fragments of |fragment| bytes
// of run-length data after the first, which holds the sizes (0: one).
std::vector<Bytes> Object(size_t fragment) {
  const Bytes rle = {1, 1, 0, 0x82, 2, 0, 0, 0, 1, 0, 0x83, 1, 0, 0};
  Bytes body = {0, 0, static_cast<uint8_t>(rle.size() + 4)};
  Put16(&body, 4);
  Put16(&body, 2);
  body.insert(body.end(), rle.begin(), rle.end());
  if (fragment == 0) {
    fragment = body.size();
  }
  std::vector<Bytes> segments;
  for (size_t at = 0; at < body.size();) {
    const size_t end = std::min(body.size(), at + fragment + (at == 0 ? 7 : 0));
    Bytes ods = {0, 0, 0, 0};
    ods[3] = static_cast<uint8_t>((at == 0 ? 0x80 : 0) |
                                  (end == body.size() ? 0x40 : 0));
    ods.insert(ods.end(), body.begin() + static_cast<ptrdiff_t>(at),
               body.begin() + static_cast<ptrdiff_t>(end));
    at = end;
    Bytes out;
    Segment(&out, 0x15, ods);
    segments.push_back(out);
  }
  return segments;
}

Bytes End() {
  Bytes out;
  Segment(&out, 0x80, {});
  return out;
}

Bytes Concat(const std::vector<Bytes>& parts) {
  Bytes out;
  for (const Bytes& part : parts) {
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

std::vector<SubtitleCue> Feed(PgsSubtitleDecoder* decoder,
                              const Bytes& data,
                              int64_t pts) {
  EncodedPacket packet;
  packet.pts = pts;
  packet.buffer = PacketBuffer::Copy(data.data(), data.size());
  std::vector<SubtitleCue> cues;
  EXPECT_TRUE(decoder->Decode(packet, &cues));
  return cues;
}

std::vector<uint8_t> Pixel(uint32_t rgba) {
  return {static_cast<uint8_t>(rgba), static_cast<uint8_t>(rgba >> 8),
          static_cast<uint8_t>(rgba >> 16), static_cast<uint8_t>(rgba >> 24)};
}

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(PgsSubtitleDecoder, Rle) {
  // One pixel of 5, three of 0, then 70 of 7 (long form) in a row of 80.
  const Bytes rle = {5, 0, 3, 0, 0xc0, 70, 7, 0, 0, 0, 0x45, 0x00, 0, 0};
  std::vector<uint8_t> indices;
  EXPECT_TRUE(
      PgsSubtitleDecoder::DecodeRle(rle.data(), rle.size(), 80, 3, &indices));
  EXPECT_EQ(indices.size(), 240u);
  EXPECT_EQ(indices[0], 5);
  EXPECT_EQ(indices[1], 0);
  EXPECT_EQ(indices[4], 7);
  EXPECT_EQ(indices[73], 7);
  EXPECT_EQ(indices[74], 0);
  // A run of 0x500 zeros, cut at the end of the row.
  EXPECT_EQ(indices[80], 0);
  // The third row is not in the data.
  EXPECT_EQ(indices[160], 0);

  // A run past the end of the row is cut there, and the row ends at 0 0.
  const Bytes over = {0, 0x85, 9, 0, 0, 3};
  EXPECT_TRUE(
      PgsSubtitleDecoder::DecodeRle(over.data(), over.size(), 2, 2, &indices));
  EXPECT_TRUE(indices == std::vector<uint8_t>({9, 9, 3, 0}));
  EXPECT_TRUE(
      !PgsSubtitleDecoder::DecodeRle(over.data(), over.size(), 0, 2, &indices));
  // Data that ends inside a run.
  const Bytes cut = {0, 0xc1};
  EXPECT_TRUE(
      PgsSubtitleDecoder::DecodeRle(cut.data(), cut.size(), 2, 1, &indices));
}

TEST(PgsSubtitleDecoder, Colors) {
  EXPECT_EQ(PgsSubtitleDecoder::ToRgba(235, 128, 128, 255, 1080), 0xffffffffu);
  EXPECT_EQ(PgsSubtitleDecoder::ToRgba(16, 128, 128, 255, 1080), 0xff000000u);
  // Premultiplied.
  EXPECT_EQ(PgsSubtitleDecoder::ToRgba(235, 128, 128, 128, 1080), 0x80808080u);
  EXPECT_EQ(PgsSubtitleDecoder::ToRgba(235, 128, 128, 0, 1080), 0u);
  // Red in BT.709 (above 576 lines) and BT.601.
  for (const uint32_t red :
       {PgsSubtitleDecoder::ToRgba(63, 240, 102, 255, 1080),
        PgsSubtitleDecoder::ToRgba(81, 240, 90, 255, 576)}) {
    EXPECT_TRUE((red & 0xff) >= 254);
    EXPECT_TRUE((red >> 8 & 0xff) < 2);
    EXPECT_TRUE((red >> 16 & 0xff) < 2);
  }
  // BT.709's red read as BT.601 is darker.
  EXPECT_TRUE((PgsSubtitleDecoder::ToRgba(63, 240, 102, 255, 576) & 0xff) <
              240);
}

TEST(PgsSubtitleDecoder, DisplaySets) {
  PgsSubtitleDecoder decoder;
  SubtitleConfig config;
  config.codec_name = "hdmv_pgs_subtitle";
  std::string error;
  EXPECT_TRUE(decoder.Initialize(config, &error));

  // An object in fragments, the first packet without its END.
  std::vector<Bytes> object = Object(5);
  EXPECT_TRUE(object.size() > 2);
  Bytes first = Concat({Composition(0x80, 3, {{100, 900}}), Palette(3)});
  first = Concat({first, object[0]});
  EXPECT_TRUE(Feed(&decoder, first, 1000000).empty());
  Bytes rest;
  for (size_t i = 1; i < object.size(); ++i) {
    rest = Concat({rest, object[i]});
  }
  std::vector<SubtitleCue> cues =
      Feed(&decoder, Concat({rest, End()}), 1000000);
  EXPECT_EQ(cues.size(), 1u);
  if (cues.size() != 1 || !cues[0].bitmaps || cues[0].bitmaps->size() != 1) {
    EXPECT_TRUE(false);
    return;
  }
  EXPECT_EQ(cues[0].start, 1000000);
  EXPECT_EQ(cues[0].end, kNoTimestamp);
  EXPECT_EQ(cues[0].canvas_width, 1920);
  EXPECT_EQ(cues[0].canvas_height, 1080);
  const SubtitleBitmap& bitmap = cues[0].bitmaps->at(0);
  EXPECT_EQ(bitmap.x, 100);
  EXPECT_EQ(bitmap.y, 900);
  EXPECT_EQ(bitmap.width, 4);
  EXPECT_EQ(bitmap.height, 2);
  const Bytes white = Pixel(0xffffffff);
  const Bytes half_black = Pixel(0x80000000);
  const Bytes clear = Pixel(0);
  EXPECT_TRUE(bitmap.rgba == Concat({white, white, half_black, half_black,
                                     clear, white, white, white}));

  // A composition of the same epoch reuses the object, cropped.
  cues =
      Feed(&decoder, Concat({Composition(0, 3, {{10, 20, 1, 0, 2, 2}}), End()}),
           2000000);
  EXPECT_EQ(cues.size(), 1u);
  if (cues.size() == 1 && cues[0].bitmaps && cues[0].bitmaps->size() == 1) {
    const SubtitleBitmap& cropped = cues[0].bitmaps->at(0);
    EXPECT_EQ(cropped.x, 10);
    EXPECT_EQ(cropped.width, 2);
    EXPECT_EQ(cropped.height, 2);
    EXPECT_TRUE(cropped.rgba == Concat({white, half_black, white, white}));
  } else {
    EXPECT_TRUE(false);
  }

  // No objects: a cue that clears the screen.
  cues = Feed(&decoder, Concat({Composition(0, 3, {}), End()}), 3000000);
  EXPECT_EQ(cues.size(), 1u);
  EXPECT_TRUE(cues.size() == 1 && !cues[0].bitmaps && cues[0].text.empty());

  // After a flush the epoch's objects are gone.
  decoder.Flush();
  cues = Feed(&decoder, Concat({Composition(0, 3, {{0, 0}}), End()}), 4000000);
  EXPECT_TRUE(cues.size() == 1 && !cues[0].bitmaps);
}

TEST(PgsSubtitleDecoder, SupHeadersAndDuration) {
  PgsSubtitleDecoder decoder;
  // Each segment after a .sup file's 10 bytes of "PG" and two times.
  Bytes data;
  for (const Bytes& segment :
       {Composition(0x80, 0, {{0, 0}}), Palette(0), Object(0)[0], End()}) {
    const Bytes header = {'P', 'G', 0, 0, 0, 0, 0, 0, 0, 0};
    data = Concat({data, header, segment});
  }
  EncodedPacket packet;
  packet.pts = 500000;
  packet.duration = 1500000;
  packet.buffer = PacketBuffer::Copy(data.data(), data.size());
  std::vector<SubtitleCue> cues;
  EXPECT_TRUE(decoder.Decode(packet, &cues));
  EXPECT_EQ(cues.size(), 1u);
  EXPECT_TRUE(cues.size() == 1 && cues[0].bitmaps);
  EXPECT_TRUE(cues.size() == 1 && cues[0].end == 2000000);
}

TEST(PgsSubtitleDecoder, Malformed) {
  PgsSubtitleDecoder decoder;
  // Lengths past the packet, an object larger than allowed, a composition
  // that names objects it does not place.
  Bytes data = {0x16, 0xff, 0xff, 0, 1};
  Feed(&decoder, data, 0);
  Bytes huge = {0x15, 0,    11,   0,    0,    0,    0x80,
                0xff, 0xff, 0xff, 0x10, 0x00, 0x10, 0x00};
  Bytes composition = Composition(0x80, 0, {{0, 0}});
  composition[13] = 9;
  std::vector<SubtitleCue> cues =
      Feed(&decoder, Concat({composition, Palette(0), huge, End()}), 0);
  EXPECT_TRUE(cues.size() == 1 && !cues[0].bitmaps);
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
