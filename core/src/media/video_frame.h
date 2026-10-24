#ifndef LYCHEE_MEDIA_VIDEO_FRAME_H_
#define LYCHEE_MEDIA_VIDEO_FRAME_H_

#include <cstdint>
#include <memory>

#include "base/time.h"

namespace lychee {

// Matches lychee_frame_type so it can be used as a bitmask.
enum class VideoFrameStorage : uint32_t {
  kCpuBgra = 1 << 0,
  // CVPixelBufferRef. Any pixel format; converters turn it into BGRA when
  // the caller asks for LYCHEE_FRAME_CVPIXELBUFFER.
  kCVPixelBuffer = 1 << 1,
};

struct VideoFrame {
  VideoFrameStorage storage = VideoFrameStorage::kCpuBgra;
  int width = 0;
  int height = 0;
  int64_t pts = kNoTimestamp;
  int64_t duration = 0;

  // kCpuBgra.
  const uint8_t* data = nullptr;
  int stride = 0;

  // kCVPixelBuffer.
  void* native_handle = nullptr;

  // Keeps |data| / |native_handle| alive.
  std::shared_ptr<void> owner;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_VIDEO_FRAME_H_
