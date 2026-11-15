// Host test double for ANativeWindow; never included by Android builds.
#pragma once
#include <cstdint>
#include <vector>
constexpr int WINDOW_FORMAT_RGBA_8888 = 1;
constexpr int ANATIVEWINDOW_TRANSFORM_IDENTITY = 0;
constexpr int ANATIVEWINDOW_TRANSFORM_ROTATE_90 = 4;
constexpr int ANATIVEWINDOW_TRANSFORM_ROTATE_180 = 3;
constexpr int ANATIVEWINDOW_TRANSFORM_ROTATE_270 = 7;
struct ANativeWindow_Buffer {
  int32_t width, height, stride, format;
  void* bits;
};
struct ANativeWindow {
  std::vector<uint8_t> pixels;
  int width = 0, height = 0, stride = 0, transform = 0;
  int geometry_error = 0, transform_error = 0, lock_error = 0, post_error = 0;
  int posts = 0;
};
inline int ANativeWindow_setBuffersGeometry(ANativeWindow* w,
                                            int width,
                                            int height,
                                            int) {
  w->width = width;
  w->height = height;
  w->stride = width + 2;
  w->pixels.assign(w->stride * height * 4, 0xCD);
  return w->geometry_error;
}
inline int ANativeWindow_setBuffersTransform(ANativeWindow* w, int transform) {
  w->transform = transform;
  return w->transform_error;
}
inline int ANativeWindow_lock(ANativeWindow* w,
                              ANativeWindow_Buffer* b,
                              void*) {
  *b = {w->width, w->height, w->stride, WINDOW_FORMAT_RGBA_8888,
        w->pixels.data()};
  return w->lock_error;
}
inline int ANativeWindow_unlockAndPost(ANativeWindow* w) {
  ++w->posts;
  return w->post_error;
}
