#include "backends/apple/pixel_buffer.h"

#include <CoreVideo/CoreVideo.h>

namespace lychee {

namespace {

bool HasAttachment(CVPixelBufferRef buffer, CFStringRef key) {
  if (__builtin_available(macOS 12, iOS 15, *)) {
    CFTypeRef value = CVBufferCopyAttachment(buffer, key, nullptr);
    if (!value) {
      return false;
    }
    CFRelease(value);
    return true;
  }
#if TARGET_OS_OSX && __MAC_OS_X_VERSION_MIN_REQUIRED < 120000
  // The framework targets macOS 11, where only the Get variant exists.
  return CVBufferGetAttachment(buffer, key, nullptr) != nullptr;
#else
  return false;  // Unreachable: macOS 12 or iOS 16 at least.
#endif
}

void AttachIfMissing(CVPixelBufferRef buffer,
                     CFStringRef key,
                     CFTypeRef value) {
  if (!HasAttachment(buffer, key)) {
    CVBufferSetAttachment(buffer, key, value,
                          kCVAttachmentMode_ShouldPropagate);
  }
}

// VideoToolbox's decoder attaches no color to a stream that has none. Its
// pixel transfer reads a missing matrix as BT.601, also for HD, and Core
// Animation shows a buffer without primaries and transfer function
// unmatched, in the display's own space (oversaturated on a P3 display).
// The library's convention is the BT.709 matrix above 576 lines and BT.601
// at or below (D3D11VideoProcessor, the dmabuf export, swscale in
// FFmpegVideoDecoder), and BT.709 primaries and transfer, as AVPlayer
// assumes too.
void AttachDefaultColor(CVPixelBufferRef buffer) {
  if (CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA) {
    return;
  }
  AttachIfMissing(buffer, kCVImageBufferYCbCrMatrixKey,
                  CVPixelBufferGetHeight(buffer) > 576
                      ? kCVImageBufferYCbCrMatrix_ITU_R_709_2
                      : kCVImageBufferYCbCrMatrix_ITU_R_601_4);
  AttachIfMissing(buffer, kCVImageBufferColorPrimariesKey,
                  kCVImageBufferColorPrimaries_ITU_R_709_2);
  AttachIfMissing(buffer, kCVImageBufferTransferFunctionKey,
                  kCVImageBufferTransferFunction_ITU_R_709_2);
}

}  // namespace

std::shared_ptr<VideoFrame> WrapPixelBuffer(void* pixel_buffer) {
  if (!pixel_buffer) {
    return nullptr;
  }
  auto buffer = static_cast<CVPixelBufferRef>(pixel_buffer);
  AttachDefaultColor(buffer);
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
