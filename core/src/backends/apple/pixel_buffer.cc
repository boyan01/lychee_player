#include "backends/apple/pixel_buffer.h"

#include <CoreVideo/CoreVideo.h>

namespace lychee {

namespace {

bool HasMatrix(CVPixelBufferRef buffer) {
  if (__builtin_available(macOS 12, iOS 15, *)) {
    CFTypeRef matrix =
        CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nullptr);
    if (!matrix) {
      return false;
    }
    CFRelease(matrix);
    return true;
  }
#if TARGET_OS_OSX && __MAC_OS_X_VERSION_MIN_REQUIRED < 120000
  // The framework targets macOS 11, where only the Get variant exists.
  return CVBufferGetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nullptr) !=
         nullptr;
#else
  return false;  // Unreachable: macOS 12 or iOS 16 at least.
#endif
}

// VideoToolbox's decoder attaches no matrix to a stream that has none, and
// its pixel transfer reads that as BT.601, also for HD. The library's
// convention is BT.709 above 576 lines (D3D11VideoProcessor, the dmabuf
// export, swscale in FFmpegVideoDecoder).
void AttachDefaultMatrix(CVPixelBufferRef buffer) {
  if (CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA) {
    return;
  }
  if (HasMatrix(buffer)) {
    return;
  }
  CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey,
                        CVPixelBufferGetHeight(buffer) > 576
                            ? kCVImageBufferYCbCrMatrix_ITU_R_709_2
                            : kCVImageBufferYCbCrMatrix_ITU_R_601_4,
                        kCVAttachmentMode_ShouldPropagate);
}

}  // namespace

std::shared_ptr<VideoFrame> WrapPixelBuffer(void* pixel_buffer) {
  if (!pixel_buffer) {
    return nullptr;
  }
  auto buffer = static_cast<CVPixelBufferRef>(pixel_buffer);
  AttachDefaultMatrix(buffer);
  CVPixelBufferRetain(buffer);
  auto frame = std::make_shared<VideoFrame>();
  frame->storage = VideoFrameStorage::kCVPixelBuffer;
  frame->width = static_cast<int>(CVPixelBufferGetWidth(buffer));
  frame->height = static_cast<int>(CVPixelBufferGetHeight(buffer));
  frame->native_handle = buffer;
  frame->owner = std::shared_ptr<void>(
      buffer, [](void* p) { CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p)); });
  return frame;
}

}  // namespace lychee
