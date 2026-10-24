#include "backends/apple/pixel_buffer.h"

#include <CoreVideo/CoreVideo.h>

namespace lychee {

std::shared_ptr<VideoFrame> WrapPixelBuffer(void* pixel_buffer) {
  if (!pixel_buffer) {
    return nullptr;
  }
  auto buffer = static_cast<CVPixelBufferRef>(pixel_buffer);
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
