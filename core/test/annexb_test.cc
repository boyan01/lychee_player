#include "media/annexb.h"

#include <vector>

#include "test.h"

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)

namespace lychee {
namespace {

VideoConfig H264(std::vector<uint8_t> extradata) {
  VideoConfig config;
  config.codec = VideoCodec::kH264;
  config.extradata = std::move(extradata);
  return config;
}

// avcC with 4-byte lengths, one SPS (67 aa) and one PPS (68 bb).
const std::vector<uint8_t> kAvcC = {1,    0x64, 0,    0x1f, 0xff, 0xe1, 0,
                                    2,    0x67, 0xaa, 1,    0,    2,    0x68,
                                    0xbb};

TEST(AnnexBConverter, PrependsParameterSetsToKeyframes) {
  AnnexBConverter converter;
  EXPECT_TRUE(converter.Initialize(H264(kAvcC)));
  EXPECT_EQ(converter.parameter_sets().size(), 2u);
  EXPECT_EQ(converter.NalType(converter.parameter_sets()[0]), 7);
  EXPECT_EQ(converter.NalType(converter.parameter_sets()[1]), 8);

  const std::vector<uint8_t> sample = {0, 0, 0, 2, 0x65, 0x11,
                                       0, 0, 0, 1, 0x06};
  std::vector<uint8_t> out;
  EXPECT_TRUE(converter.Convert(sample.data(), sample.size(), true, &out));
  const std::vector<uint8_t> expected = {
      0, 0, 0, 1, 0x67, 0xaa, 0, 0, 0, 1, 0x68, 0xbb,
      0, 0, 0, 1, 0x65, 0x11, 0, 0, 0, 1, 0x06};
  EXPECT_TRUE(out == expected);

  out.clear();
  EXPECT_TRUE(converter.Convert(sample.data(), sample.size(), false, &out));
  EXPECT_EQ(out.size(), 11u);
  EXPECT_EQ(out[4], 0x65);
}

TEST(AnnexBConverter, RejectsTruncatedSamples) {
  AnnexBConverter converter;
  EXPECT_TRUE(converter.Initialize(H264(kAvcC)));
  const std::vector<uint8_t> sample = {0, 0, 0, 9, 0x65, 0x11};
  std::vector<uint8_t> out;
  EXPECT_TRUE(!converter.Convert(sample.data(), sample.size(), false, &out));
}

TEST(AnnexBConverter, PassesAnnexBThrough) {
  AnnexBConverter converter;
  EXPECT_TRUE(converter.Initialize(H264({0, 0, 1, 0x67, 0xaa, 0, 0, 0, 1,
                                         0x68, 0xbb})));
  EXPECT_EQ(converter.parameter_sets().size(), 2u);
  const std::vector<uint8_t> sample = {0, 0, 1, 0x41, 0x22};
  std::vector<uint8_t> out;
  EXPECT_TRUE(converter.Convert(sample.data(), sample.size(), false, &out));
  EXPECT_TRUE(out == sample);
}

TEST(AnnexBConverter, ParsesHvcC) {
  std::vector<uint8_t> hvcc(23, 0);
  hvcc[0] = 1;
  hvcc[21] = 0xff;  // lengthSizeMinusOne = 3
  hvcc[22] = 2;     // arrays: VPS, SPS
  for (const uint8_t type : {uint8_t{32}, uint8_t{33}}) {
    const uint8_t header = static_cast<uint8_t>(type << 1);
    hvcc.insert(hvcc.end(), {type, 0, 1, 0, 3, header, 1, 0xcc});
  }
  VideoConfig config;
  config.codec = VideoCodec::kHEVC;
  config.extradata = hvcc;
  AnnexBConverter converter;
  EXPECT_TRUE(converter.Initialize(config));
  EXPECT_EQ(converter.parameter_sets().size(), 2u);
  EXPECT_EQ(converter.NalType(converter.parameter_sets()[0]), 32);
  EXPECT_EQ(converter.NalType(converter.parameter_sets()[1]), 33);
}

TEST(AnnexBConverter, RejectsOtherCodecs) {
  VideoConfig config;
  config.codec = VideoCodec::kVP9;
  AnnexBConverter converter;
  EXPECT_TRUE(!converter.Initialize(config));
}

using Picture = AnnexBConverter::Picture;

TEST(AnnexBConverter, ClassifiesH264Pictures) {
  AnnexBConverter converter;
  EXPECT_TRUE(converter.Initialize(H264(kAvcC)));
  // An SEI, then an IDR slice.
  const std::vector<uint8_t> idr = {0, 0, 0, 2, 0x06, 0x05,
                                    0, 0, 0, 2, 0x65, 0x88};
  EXPECT_TRUE(converter.Classify(idr.data(), idr.size()) == Picture::kKeyframe);
  // Non-IDR slices: first_mb_in_slice 0, then slice_type 7 (I), 0 (P)
  // and 1 (B).
  const std::vector<uint8_t> intra = {0, 0, 0, 2, 0x41, 0x88};
  EXPECT_TRUE(converter.Classify(intra.data(), intra.size()) ==
              Picture::kIntra);
  const std::vector<uint8_t> p = {0, 0, 0, 2, 0x41, 0xc0};
  EXPECT_TRUE(converter.Classify(p.data(), p.size()) == Picture::kInter);
  const std::vector<uint8_t> b = {0, 0, 0, 2, 0x01, 0xa0};
  EXPECT_TRUE(converter.Classify(b.data(), b.size()) == Picture::kInter);
  // No slice, and a slice header cut short.
  const std::vector<uint8_t> sei = {0, 0, 0, 2, 0x06, 0x05};
  EXPECT_TRUE(converter.Classify(sei.data(), sei.size()) == Picture::kUnknown);
  const std::vector<uint8_t> cut = {0, 0, 0, 2, 0x41, 0x00};
  EXPECT_TRUE(converter.Classify(cut.data(), cut.size()) == Picture::kUnknown);
}

TEST(AnnexBConverter, ClassifiesAcrossEmulationPrevention) {
  // first_mb_in_slice with 23 leading zero bits: its RBSP starts
  // 00 00 01, written 00 00 03 01. Then slice_type 2 (I).
  const uint8_t slice[] = {0x41, 0, 0, 3, 1, 0xff, 0xff, 0xfe, 0xe0};
  EXPECT_TRUE(AnnexBConverter::ClassifyNal(slice, sizeof(slice), false) ==
              Picture::kIntra);
}

TEST(AnnexBConverter, ClassifiesHevcPictures) {
  // IDR_W_RADL and CRA are IRAP; TRAIL_R is not; VPS and SEI are no
  // slices.
  const uint8_t idr[] = {0x26, 0x01};
  const uint8_t cra[] = {0x2a, 0x01};
  const uint8_t trail[] = {0x02, 0x01};
  const uint8_t vps[] = {0x40, 0x01};
  EXPECT_TRUE(AnnexBConverter::ClassifyNal(idr, 2, true) == Picture::kKeyframe);
  EXPECT_TRUE(AnnexBConverter::ClassifyNal(cra, 2, true) == Picture::kKeyframe);
  EXPECT_TRUE(AnnexBConverter::ClassifyNal(trail, 2, true) == Picture::kInter);
  EXPECT_TRUE(AnnexBConverter::ClassifyNal(vps, 2, true) == Picture::kUnknown);
}

}  // namespace
}  // namespace lychee

// NOLINTEND(bugprone-throwing-static-initialization)
