#define LOG_TAG "apple_converter"

#include "backends/apple/apple_frame_converter.h"

#include <cstring>
#include <vector>

#include "backends/apple/pixel_buffer.h"
#include "base/log.h"

namespace lychee {

namespace {

bool Accepts(uint32_t accepted, VideoFrameStorage storage) {
  return (accepted & static_cast<uint32_t>(storage)) != 0;
}

bool IsBgra(CVPixelBufferRef buffer) {
  return CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA;
}

void SetNumber(CFMutableDictionaryRef dictionary, CFStringRef key, int value) {
  CFNumberRef number = CFNumberCreate(nullptr, kCFNumberIntType, &value);
  CFDictionarySetValue(dictionary, key, number);
  CFRelease(number);
}

void CopyRows(const uint8_t* source,
              size_t source_stride,
              uint8_t* destination,
              size_t destination_stride,
              size_t row_bytes,
              int rows) {
  for (int y = 0; y < rows; ++y) {
    memcpy(destination + y * destination_stride, source + y * source_stride,
           row_bytes);
  }
}

}  // namespace

AppleFrameConverter::AppleFrameConverter() = default;

AppleFrameConverter::~AppleFrameConverter() {
  if (transfer_session_) {
    VTPixelTransferSessionInvalidate(transfer_session_);
    CFRelease(transfer_session_);
  }
  CVPixelBufferPoolRelease(pool_);
}

std::shared_ptr<VideoFrame> AppleFrameConverter::Convert(
    const std::shared_ptr<VideoFrame>& frame,
    uint32_t accepted) {
  if (frame->storage == VideoFrameStorage::kCVPixelBuffer &&
      Accepts(accepted, VideoFrameStorage::kCVPixelBuffer)) {
    return ToBgraPixelBuffer(frame);
  }
  if (frame->storage == VideoFrameStorage::kCpuBgra &&
      Accepts(accepted, VideoFrameStorage::kCpuBgra)) {
    return frame;
  }
  if (Accepts(accepted, VideoFrameStorage::kCVPixelBuffer)) {
    return ToBgraPixelBuffer(frame);
  }
  if (Accepts(accepted, VideoFrameStorage::kCpuBgra)) {
    return ToCpuBgra(frame);
  }
  if (Accepts(accepted, VideoFrameStorage::kCpuRgba)) {
    std::shared_ptr<VideoFrame> bgra =
        frame->storage == VideoFrameStorage::kCVPixelBuffer ? ToCpuBgra(frame)
                                                            : frame;
    return bgra ? cpu_converter_.Convert(bgra, accepted) : nullptr;
  }
  return nullptr;
}

CVPixelBufferRef AppleFrameConverter::CreateBgraBuffer(int width, int height) {
  if (!pool_ || pool_width_ != width || pool_height_ != height) {
    CVPixelBufferPoolRelease(pool_);
    pool_ = nullptr;
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    SetNumber(attributes, kCVPixelBufferPixelFormatTypeKey,
              kCVPixelFormatType_32BGRA);
    SetNumber(attributes, kCVPixelBufferWidthKey, width);
    SetNumber(attributes, kCVPixelBufferHeightKey, height);
    CFDictionaryRef io_surface = CFDictionaryCreate(
        nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey,
                         io_surface);
    CFRelease(io_surface);
    CFDictionarySetValue(attributes, kCVPixelBufferMetalCompatibilityKey,
                         kCFBooleanTrue);
    CVReturn status =
        CVPixelBufferPoolCreate(nullptr, nullptr, attributes, &pool_);
    CFRelease(attributes);
    if (status != kCVReturnSuccess) {
      LOGE("CVPixelBufferPoolCreate failed: %d", status);
      pool_ = nullptr;
      return nullptr;
    }
    pool_width_ = width;
    pool_height_ = height;
  }
  CVPixelBufferRef buffer = nullptr;
  CVReturn status = CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_, &buffer);
  if (status != kCVReturnSuccess) {
    LOGE("CVPixelBufferPoolCreatePixelBuffer failed: %d", status);
    return nullptr;
  }
  return buffer;
}

std::shared_ptr<VideoFrame> AppleFrameConverter::ToBgraPixelBuffer(
    const std::shared_ptr<VideoFrame>& frame) {
  CVPixelBufferRef output = nullptr;
  if (frame->storage == VideoFrameStorage::kCVPixelBuffer) {
    auto input = static_cast<CVPixelBufferRef>(frame->native_handle);
    if (IsBgra(input)) {
      return frame;
    }
    if (!transfer_session_ &&
        VTPixelTransferSessionCreate(nullptr, &transfer_session_) != noErr) {
      LOGE("VTPixelTransferSessionCreate failed");
      transfer_session_ = nullptr;
      return nullptr;
    }
    output = CreateBgraBuffer(frame->width, frame->height);
    if (!output) {
      return nullptr;
    }
    OSStatus status =
        VTPixelTransferSessionTransferImage(transfer_session_, input, output);
    if (status != noErr) {
      LOGE("VTPixelTransferSessionTransferImage failed: %d",
           static_cast<int>(status));
      CVPixelBufferRelease(output);
      return nullptr;
    }
  } else {
    output = CreateBgraBuffer(frame->width, frame->height);
    if (!output) {
      return nullptr;
    }
    CVPixelBufferLockBaseAddress(output, 0);
    CopyRows(frame->data, frame->stride,
             static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(output)),
             CVPixelBufferGetBytesPerRow(output),
             static_cast<size_t>(frame->width) * 4, frame->height);
    CVPixelBufferUnlockBaseAddress(output, 0);
  }
  std::shared_ptr<VideoFrame> result = WrapPixelBuffer(output);
  CVPixelBufferRelease(output);
  result->pts = frame->pts;
  result->duration = frame->duration;
  return result;
}

std::shared_ptr<VideoFrame> AppleFrameConverter::ToCpuBgra(
    const std::shared_ptr<VideoFrame>& frame) {
  std::shared_ptr<VideoFrame> bgra = ToBgraPixelBuffer(frame);
  if (!bgra) {
    return nullptr;
  }
  auto buffer = static_cast<CVPixelBufferRef>(bgra->native_handle);
  size_t stride = static_cast<size_t>(bgra->width) * 4;
  auto pixels = std::make_shared<std::vector<uint8_t>>(stride * bgra->height);
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  CopyRows(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer)),
           CVPixelBufferGetBytesPerRow(buffer), pixels->data(), stride, stride,
           bgra->height);
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);

  auto result = std::make_shared<VideoFrame>();
  result->storage = VideoFrameStorage::kCpuBgra;
  result->width = bgra->width;
  result->height = bgra->height;
  result->pts = frame->pts;
  result->duration = frame->duration;
  result->data = pixels->data();
  result->stride = static_cast<int>(stride);
  result->owner = std::move(pixels);
  return result;
}

}  // namespace lychee
