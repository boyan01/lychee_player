#if defined(__APPLE__)

#include "backends/apple/apple_frame_converter.h"
#include "backends/apple/pixel_buffer.h"

#include <CoreVideo/CoreVideo.h>

#include <vector>

#include "media/stream_info.h"
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

// Registration uses the test harness's process-lifetime vector.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(AppleFrameConverter, SourceColorReplacesGuessesAndKeepsExplicitFields) {
  // Untagged SD, a partially tagged SMPTE-C source, and explicitly tagged HDR.
  for (const ColorInfo& source :
       {ColorInfo{}, ColorInfo{6, 2, 2, false}, ColorInfo{9, 16, 9, false}}) {
    CVPixelBufferRef buffer = nullptr;
    EXPECT_EQ(
        CVPixelBufferCreate(nullptr, 16, 16,
                            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                            nullptr, &buffer),
        kCVReturnSuccess);
    if (!buffer) {
      continue;
    }
    const void* keys[] = {kCVImageBufferColorPrimariesKey,
                          kCVImageBufferTransferFunctionKey,
                          kCVImageBufferYCbCrMatrixKey};
    const void* values[] = {kCVImageBufferColorPrimaries_SMPTE_C,
                            kCVImageBufferTransferFunction_ITU_R_709_2,
                            kCVImageBufferYCbCrMatrix_ITU_R_601_4};
    CFDictionaryRef guessed_tags = CFDictionaryCreate(
        nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CGColorSpaceRef guessed =
        CVImageBufferCreateColorSpaceFromAttachments(guessed_tags);
    CFRelease(guessed_tags);
    CVBufferSetAttachment(buffer, kCVImageBufferCGColorSpaceKey, guessed,
                          kCVAttachmentMode_ShouldPropagate);
    auto frame = WrapPixelBuffer(buffer, &source);
    EXPECT_TRUE(frame != nullptr);
    CFDictionaryRef attachments =
        CVBufferCopyAttachments(buffer, kCVAttachmentMode_ShouldPropagate);
    const auto primaries = static_cast<CFStringRef>(
        CFDictionaryGetValue(attachments, kCVImageBufferColorPrimariesKey));
    const auto transfer = static_cast<CFStringRef>(
        CFDictionaryGetValue(attachments, kCVImageBufferTransferFunctionKey));
    const auto matrix = static_cast<CFStringRef>(
        CFDictionaryGetValue(attachments, kCVImageBufferYCbCrMatrixKey));
    EXPECT_TRUE(
        primaries &&
        CFEqual(primaries, CVColorPrimariesGetStringForIntegerCodePoint(
                               source.primaries == 2 ? 1 : source.primaries)));
    EXPECT_TRUE(
        transfer &&
        CFEqual(transfer, CVTransferFunctionGetStringForIntegerCodePoint(
                              source.transfer == 2 ? 1 : source.transfer)));
    EXPECT_TRUE(matrix &&
                CFEqual(matrix, CVYCbCrMatrixGetStringForIntegerCodePoint(
                                    source.matrix == 2 ? 6 : source.matrix)));
    if (source.primaries == 2) {
      const void* space =
          CFDictionaryGetValue(attachments, kCVImageBufferCGColorSpaceKey);
      EXPECT_TRUE(space && !CFEqual(space, guessed));
    }
    CFRelease(guessed);
    CFRelease(attachments);
    CVPixelBufferRelease(buffer);
  }
}

}  // namespace
}  // namespace lychee

#endif  // defined(__APPLE__)
