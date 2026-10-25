// Shows a player's frames in a Flutter SurfaceProducer.
//
// Hardware-decoded frames (LYCHEE_FRAME_ANDROID_SURFACE) are rendered by
// MediaCodec straight into the window given to the player with
// lychee_player_set_video_surface(); acquiring a frame releases it there.
// Software-decoded frames (LYCHEE_FRAME_CPU_RGBA) are copied into another
// window: a window a CPU has drawn into stays connected to the CPU, and
// MediaCodec can no longer render into it.
//
// LycheeTexture.kt drives it: Render() once per vsync from its Choreographer
// thread, the setters and Detach() from the platform thread. The mutex
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
  // Where CPU frames are drawn.
  ANativeWindow* cpu_window = nullptr;
  // Draw the current frame again even if the player has no new one: a new
  // window starts out empty.
  bool redraw = false;
  int32_t frame_type = 0;
  int32_t frame_width = 0;
  int32_t frame_height = 0;
};

Texture* FromHandle(jlong handle) {
  return reinterpret_cast<Texture*>(handle);
}

void ReleaseCpuWindowLocked(Texture* texture) {
  if (texture->cpu_window) {
    ANativeWindow_release(texture->cpu_window);
    texture->cpu_window = nullptr;
  }
}

// Returns false if the window is not the frame's size yet; the caller
// resizes the SurfaceProducer, and the next window is.
bool DrawLocked(Texture* texture, lychee_frame* frame) {
  const int32_t width = lychee_frame_get_width(frame);
  const int32_t height = lychee_frame_get_height(frame);
  ANativeWindow* window = texture->cpu_window;
  if (!window || ANativeWindow_getWidth(window) != width ||
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

// The window MediaCodec renders into; null detaches it.
JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeSetOutput(JNIEnv* env,
                                                              jclass,
                                                              jlong handle,
                                                              jobject surface) {
  Texture* texture = FromHandle(handle);
  std::lock_guard<std::mutex> lock(texture->mutex);
  if (!texture->player) {
    return;
  }
  ANativeWindow* window =
      surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
  // The player takes its own reference.
  lychee_player_set_video_surface(texture->player, window);
  if (window) {
    ANativeWindow_release(window);
  }
}

// The window CPU frames are copied into; null detaches it.
JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeSetCpuSurface(
    JNIEnv* env,
    jclass,
    jlong handle,
    jobject surface) {
  Texture* texture = FromHandle(handle);
  std::lock_guard<std::mutex> lock(texture->mutex);
  ReleaseCpuWindowLocked(texture);
  if (surface && texture->player) {
    texture->cpu_window = ANativeWindow_fromSurface(env, surface);
  }
  texture->redraw = true;
}

// Returns the latest frame's type and size as
// (type << 48 | width << 24 | height), 0 before the first frame.
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
  if (changed || texture->redraw) {
    // Acquiring a surface frame is what shows it.
    lychee_frame* frame =
        lychee_player_acquire_frame(texture->player, display_time_us);
    if (frame) {
      texture->frame_type = lychee_frame_get_type(frame);
      texture->frame_width = lychee_frame_get_width(frame);
      texture->frame_height = lychee_frame_get_height(frame);
      texture->redraw = texture->frame_type == LYCHEE_FRAME_CPU_RGBA &&
                        !DrawLocked(texture, frame);
      lychee_frame_release(frame);
    }
  }
  return (static_cast<jlong>(texture->frame_type) << 48) |
         (static_cast<jlong>(texture->frame_width & 0xffffff) << 24) |
         (texture->frame_height & 0xffffff);
}

// After this returns, the player is not touched again.
JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeDetach(JNIEnv*,
                                                           jclass,
                                                           jlong handle) {
  Texture* texture = FromHandle(handle);
  std::lock_guard<std::mutex> lock(texture->mutex);
  if (texture->player) {
    lychee_player_set_video_surface(texture->player, nullptr);
  }
  texture->player = nullptr;
  ReleaseCpuWindowLocked(texture);
}

JNIEXPORT void JNICALL
Java_dev_lychee_lychee_1player_LycheeTexture_nativeDestroy(JNIEnv*,
                                                            jclass,
                                                            jlong handle) {
  delete FromHandle(handle);
}

}  // extern "C"
