#include "backends/apple/pixel_buffer.h"

#include <CoreVideo/CoreVideo.h>

#include "media/stream_info.h"

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

// VideoToolbox can guess Composite NTSC for untagged SD. That CGColorSpace
// takes precedence in Core Animation, so use source metadata when known,
// including our defaults for its unspecified fields. Explicit source tags
// still win, even when only some fields were specified.
// The library's convention is the BT.709 matrix above 576 lines and BT.601
// at or below (D3D11VideoProcessor, the dmabuf export, swscale in
// FFmpegVideoDecoder), and BT.709 primaries and transfer for untagged video.
void AttachDefaultColor(CVPixelBufferRef buffer, const ColorInfo* color) {
  if (CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA) {
    return;
  }
  CFStringRef matrix = CVPixelBufferGetHeight(buffer) > 576
                           ? kCVImageBufferYCbCrMatrix_ITU_R_709_2
                           : kCVImageBufferYCbCrMatrix_ITU_R_601_4;
  if (color) {
    CFStringRef primaries = CVColorPrimariesGetStringForIntegerCodePoint(
        color->primaries == 2 ? 1 : color->primaries);
    CFStringRef transfer = CVTransferFunctionGetStringForIntegerCodePoint(
        color->transfer == 2 ? 1 : color->transfer);
    if (CFStringRef specified =
            CVYCbCrMatrixGetStringForIntegerCodePoint(color->matrix)) {
      matrix = specified;
    }
    const void* keys[] = {kCVImageBufferColorPrimariesKey,
                          kCVImageBufferTransferFunctionKey,
                          kCVImageBufferYCbCrMatrixKey};
    const void* values[] = {
        primaries ? primaries : kCVImageBufferColorPrimaries_ITU_R_709_2,
        transfer ? transfer : kCVImageBufferTransferFunction_ITU_R_709_2,
        matrix};
    CFDictionaryRef attachments = CFDictionaryCreate(
        nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CVBufferSetAttachments(buffer, attachments,
                           kCVAttachmentMode_ShouldPropagate);
    CGColorSpaceRef space =
        CVImageBufferCreateColorSpaceFromAttachments(attachments);
    CFRelease(attachments);
    if (space) {
      CVBufferSetAttachment(buffer, kCVImageBufferCGColorSpaceKey, space,
                            kCVAttachmentMode_ShouldPropagate);
      CFRelease(space);
    } else {
      CVBufferRemoveAttachment(buffer, kCVImageBufferCGColorSpaceKey);
    }
    return;
  }
  AttachIfMissing(buffer, kCVImageBufferYCbCrMatrixKey, matrix);
  AttachIfMissing(buffer, kCVImageBufferColorPrimariesKey,
                  kCVImageBufferColorPrimaries_ITU_R_709_2);
  AttachIfMissing(buffer, kCVImageBufferTransferFunctionKey,
                  kCVImageBufferTransferFunction_ITU_R_709_2);
}

}  // namespace

std::shared_ptr<VideoFrame> WrapPixelBuffer(void* pixel_buffer,
                                            const ColorInfo* color) {
  if (!pixel_buffer) {
    return nullptr;
  }
  auto buffer = static_cast<CVPixelBufferRef>(pixel_buffer);
  AttachDefaultColor(buffer, color);
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
