#include "media/cpu_frame_converter.h"

#include <vector>

#include "test.h"

namespace lychee {
namespace {

std::shared_ptr<VideoFrame> MakeBgra(std::vector<uint8_t>* pixels) {
  auto frame = std::make_shared<VideoFrame>();
  frame->storage = VideoFrameStorage::kCpuBgra;
  frame->width = 2;
  frame->height = 2;
  // Rows padded to 12 bytes, as decoders align them.
  frame->stride = 12;
  *pixels = {
      1, 2, 3, 4, 5, 6, 7, 8, 0xee, 0xee, 0xee, 0xee,
      9, 10, 11, 12, 13, 14, 15, 16, 0xee, 0xee, 0xee, 0xee,
  };
  frame->data = pixels->data();
  frame->pts = 1234;
  return frame;
}

TEST(CpuFrameConverter, SwapsRedAndBlue) {
  std::vector<uint8_t> pixels;
  auto bgra = MakeBgra(&pixels);
  CpuFrameConverter converter;
  auto rgba = converter.Convert(
      bgra, static_cast<uint32_t>(VideoFrameStorage::kCpuRgba));
  EXPECT_TRUE(rgba != nullptr);
  EXPECT_TRUE(rgba->storage == VideoFrameStorage::kCpuRgba);
  EXPECT_EQ(rgba->pts, 1234);
  EXPECT_EQ(rgba->stride, 8);
  const std::vector<uint8_t> expected = {3,  2,  1,  4,  7,  6,  5,  8,
                                         11, 10, 9,  12, 15, 14, 13, 16};
  EXPECT_TRUE(std::vector<uint8_t>(rgba->data, rgba->data + 16) == expected);
}

TEST(CpuFrameConverter, PassesAcceptedFramesThrough) {
  std::vector<uint8_t> pixels;
  auto bgra = MakeBgra(&pixels);
  CpuFrameConverter converter;
  const uint32_t both = static_cast<uint32_t>(VideoFrameStorage::kCpuBgra) |
                        static_cast<uint32_t>(VideoFrameStorage::kCpuRgba);
  EXPECT_TRUE(converter.Convert(bgra, both) == bgra);
  EXPECT_TRUE(converter.Convert(
                  bgra, static_cast<uint32_t>(
                            VideoFrameStorage::kCVPixelBuffer)) == nullptr);
}

}  // namespace
}  // namespace lychee
