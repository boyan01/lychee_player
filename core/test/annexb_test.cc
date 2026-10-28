#include "media/annexb.h"

#include <vector>

#include "test.h"

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

}  // namespace
}  // namespace lychee
