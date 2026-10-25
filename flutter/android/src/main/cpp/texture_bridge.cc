// Draws a player's frames into the Surface of a Flutter SurfaceProducer.
//
// LycheeTexture.kt drives it: Render() once per vsync from its Choreographer
// thread, SetSurface() and Detach() from the platform thread. The mutex
// keeps those apart, so after Detach() returns the player is no longer used.

#include <android/native_window_jni.h>
#include <jni.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "lychee/lychee.h"

namespace {

struct Texture {
  std::mutex mutex;
  lychee_player* player = nullptr;
  ANativeWindow* window = nullptr;
  // Draw the current frame again even if the player has no new one: a new
  // window starts out empty.
  bool redraw = false;
  int32_t frame_width = 0;
  int32_t frame_height = 0;
};

Texture* FromHandle(jlong handle) {
  return reinterpret_cast<Texture*>(handle);
}

void ReleaseWindowLocked(Texture* texture) {
  if (texture->window) {
    ANativeWindow_release(texture->window);
    texture->window = nullptr;
  }
}

// Returns false if the window is not the frame's size yet; the caller
// resizes the SurfaceProducer, and the next window is.
bool DrawLocked(Texture* texture, lychee_frame* frame) {
  const int32_t width = lychee_frame_get_width(frame);
  const int32_t height = lychee_frame_get_height(frame);
  texture->frame_width = width;
  texture->frame_height = height;
  ANativeWindow* window = texture->window;
  if (ANativeWindow_getWidth(window) != width ||
      ANativeWindow_getHeight(window) != height) {
    return false;
  }
  ANativeWindow_setBuffersGeometry(window, 0, 0, WINDOW_FORMAT_RGBA_8888);
  ANativeWindow_Buffer buffer;
  if (ANativeWindow_lock(window, &buffer, nullptr) != 0) {
    return false;
  }
  int32_t stride = 0;
  const uint8_t* source = lychee_frame_get_data(frame, &stride);
  if (source) {
    auto* dest = static_cast<uint8_t*>(buffer.bits);
    const size_t row = static_cast<size_t>(std::min(width, buffer.width)) * 4;
    const int32_t rows = std::min(height, buffer.height);
    for (int32_t y = 0; y < rows; ++y) {
      std::memcpy(dest + static_cast<size_t>(y) * buffer.stride * 4,
                  source + static_cast<size_t>(y) * stride, row);
    }
  }
  ANativeWindow_unlockAndPost(window);
  return true;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeCreate(JNIEnv*,
                                                           jclass,
                                                           jlong player) {
  auto* texture = new Texture();
  texture->player = reinterpret_cast<lychee_player*>(player);
  return reinterpret_cast<jlong>(texture);
}

JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeSetSurface(JNIEnv* env,
                                                               jclass,
                                                               jlong handle,
                                                               jobject surface) {
  Texture* texture = FromHandle(handle);
  std::lock_guard<std::mutex> lock(texture->mutex);
  ReleaseWindowLocked(texture);
  if (surface && texture->player) {
    texture->window = ANativeWindow_fromSurface(env, surface);
  }
  texture->redraw = true;
}

// Returns the size of the latest frame as (width << 32 | height), 0 before
// the first one.
JNIEXPORT jlong JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeRender(JNIEnv*,
                                                           jclass,
                                                           jlong handle,
                                                           jlong display_time_us) {
  Texture* texture = FromHandle(handle);
  std::lock_guard<std::mutex> lock(texture->mutex);
  if (!texture->player) {
    return 0;
  }
  // Called every vsync, drawn or not: the player estimates the display's
  // refresh interval from these calls.
  const bool changed =
      lychee_player_has_new_frame(texture->player, display_time_us) != 0;
  if ((changed || texture->redraw) && texture->window) {
    lychee_frame* frame =
        lychee_player_acquire_frame(texture->player, display_time_us);
    if (frame) {
      texture->redraw = !DrawLocked(texture, frame);
      lychee_frame_release(frame);
    }
  }
  return (static_cast<jlong>(texture->frame_width) << 32) |
         static_cast<uint32_t>(texture->frame_height);
}

// After this returns, the player is not touched again.
JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeDetach(JNIEnv*,
                                                           jclass,
                                                           jlong handle) {
  Texture* texture = FromHandle(handle);
  std::lock_guard<std::mutex> lock(texture->mutex);
  texture->player = nullptr;
  ReleaseWindowLocked(texture);
}

JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeDestroy(JNIEnv*,
                                                            jclass,
                                                            jlong handle) {
  delete FromHandle(handle);
}

}  // extern "C"
