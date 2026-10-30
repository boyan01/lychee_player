#ifndef LYCHEE_MEDIA_VIDEO_FRAME_H_
#define LYCHEE_MEDIA_VIDEO_FRAME_H_

#include <cstdint>
#include <functional>
#include <memory>

#include "base/time.h"

namespace lychee {

// Matches lychee_frame_type so it can be used as a bitmask.
enum class VideoFrameStorage : uint32_t {
  kCpuBgra = 1 << 0,
  // CVPixelBufferRef. Any pixel format; converters turn it into BGRA when
  // the caller asks for LYCHEE_FRAME_CVPIXELBUFFER.
  kCVPixelBuffer = 1 << 1,
  kCpuRgba = 1 << 2,
  // Already in the decoder's output Surface; |present| releases it there.
  kAndroidSurface = 1 << 3,
  // ID3D11Texture2D, shared BGRA.
  kD3D11Texture = 1 << 4,
  // lychee_dmabuf.
  kDmabuf = 1 << 5,
  // CVPixelBufferRef in the decoder's format, handed out as is.
  kCVPixelBufferNative = 1 << 6,
  // ID3D11Texture2D, shared R10G10B10A2, full-range PQ / BT.2020.
  kD3D11TextureHdr = 1 << 7,
};

struct VideoFrame {
  VideoFrameStorage storage = VideoFrameStorage::kCpuBgra;
  int width = 0;
  int height = 0;
  int64_t pts = kNoTimestamp;
  int64_t duration = 0;

  // kCpuBgra and kCpuRgba.
  const uint8_t* data = nullptr;
  int stride = 0;

  // kCVPixelBuffer, kD3D11Texture, kDmabuf.
  void* native_handle = nullptr;

  // Called once, under the renderer's lock, when the frame is first handed
  // out for |display_time| (lychee_now_us() timebase). For decoders that
  // present by releasing output buffers (kAndroidSurface).
  std::function<void(int64_t display_time)> present;

  // Keeps |data| / |native_handle| alive.
  std::shared_ptr<void> owner;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_VIDEO_FRAME_H_
