#if defined(__APPLE__)

#include "backends/apple/apple_frame_converter.h"

#include <CoreVideo/CoreVideo.h>

#include <vector>

#include "test.h"

namespace lychee {
namespace {

// Software decoding can make RGBA; a caller that takes pixel buffers and
// RGBA, but not BGRA, gets a BGRA pixel buffer of the same colors.
TEST(AppleFrameConverter, RgbaToPixelBufferKeepsColors) {
  std::vector<uint8_t> pixels = {255, 0, 0, 255, 255, 0, 0, 255,
                                 255, 0, 0, 255, 255, 0, 0, 255};
  auto frame = std::make_shared<VideoFrame>();
  frame->storage = VideoFrameStorage::kCpuRgba;
  frame->width = 2;
  frame->height = 2;
  frame->stride = 8;
  frame->data = pixels.data();
  AppleFrameConverter converter;
  auto result = converter.Convert(
      frame, static_cast<uint32_t>(VideoFrameStorage::kCVPixelBuffer) |
                 static_cast<uint32_t>(VideoFrameStorage::kCpuRgba));
  EXPECT_TRUE(result != nullptr);
  if (!result) {
    return;
  }
  EXPECT_TRUE(result->storage == VideoFrameStorage::kCVPixelBuffer);
  auto buffer = static_cast<CVPixelBufferRef>(result->native_handle);
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  const auto* bgra =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  // Red in BGRA order.
  EXPECT_EQ(int{bgra[0]}, 0);
  EXPECT_EQ(int{bgra[1]}, 0);
  EXPECT_EQ(int{bgra[2]}, 255);
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
}

}  // namespace
}  // namespace lychee

#endif  // defined(__APPLE__)
