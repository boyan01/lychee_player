#ifndef LYCHEE_ANDROID_SURFACE_FRAME_H_
#define LYCHEE_ANDROID_SURFACE_FRAME_H_

#include <android/native_window.h>

#include <cstdint>
#include <cstring>

namespace lychee::android {

enum class DrawResult { kPending, kPosted, kFailed };

inline int32_t Transform(int32_t rotation) {
  switch ((rotation % 360 + 360) % 360) {
    case 90:
      return ANATIVEWINDOW_TRANSFORM_ROTATE_90;
    case 180:
      return ANATIVEWINDOW_TRANSFORM_ROTATE_180;
    case 270:
      return ANATIVEWINDOW_TRANSFORM_ROTATE_270;
    default:
      return ANATIVEWINDOW_TRANSFORM_IDENTITY;
  }
}

inline void WindowSize(int32_t rotation,
                       int32_t width,
                       int32_t height,
                       int32_t* window_width,
                       int32_t* window_height) {
  const bool sideways = rotation % 180 != 0;
  *window_width = sideways ? height : width;
  *window_height = sideways ? width : height;
}

// Copy into the actual SurfaceView's ANativeWindow. Window dimensions are
// captured on binding, before setBuffersGeometry changes the buffer dimensions.
// A size mismatch requests another surface; producer errors must not look like
// a successfully rendered frame or retry forever on a black surface.
inline DrawResult DrawRgbaFrame(ANativeWindow* window,
                                int32_t window_width,
                                int32_t window_height,
                                const uint8_t* source,
                                int32_t stride,
                                int32_t width,
                                int32_t height,
                                int32_t rotation) {
  int32_t expected_width = 0, expected_height = 0;
  WindowSize(rotation, width, height, &expected_width, &expected_height);
  if (!window || window_width != expected_width ||
      window_height != expected_height) {
    return DrawResult::kPending;
  }
  if (!source || width <= 0 || height <= 0 || stride < int64_t{width} * 4 ||
      ANativeWindow_setBuffersGeometry(window, width, height,
                                       WINDOW_FORMAT_RGBA_8888) != 0 ||
      ANativeWindow_setBuffersTransform(window, Transform(rotation)) != 0) {
    return DrawResult::kFailed;
  }
  ANativeWindow_Buffer buffer{};
  if (ANativeWindow_lock(window, &buffer, nullptr) != 0)
    return DrawResult::kFailed;
  const bool valid = buffer.bits && buffer.width >= width &&
                     buffer.height >= height && buffer.stride >= width &&
                     buffer.format == WINDOW_FORMAT_RGBA_8888;
  if (valid) {
    auto* dest = static_cast<uint8_t*>(buffer.bits);
    const size_t dest_stride = static_cast<size_t>(buffer.stride) * 4;
    const size_t row = static_cast<size_t>(width) * 4;
    if (static_cast<size_t>(stride) == dest_stride) {
      std::memcpy(dest, source,
                  dest_stride * static_cast<size_t>(height - 1) + row);
    } else {
      for (int32_t y = 0; y < height; ++y) {
        std::memcpy(dest + static_cast<size_t>(y) * dest_stride,
                    source + static_cast<size_t>(y) * stride, row);
      }
    }
  }
  const int posted = ANativeWindow_unlockAndPost(window);
  return valid && posted == 0 ? DrawResult::kPosted : DrawResult::kFailed;
}

}  // namespace lychee::android
#endif  // LYCHEE_ANDROID_SURFACE_FRAME_H_
