#ifndef FLUTTER_PLUGIN_LYCHEE_TEXTURE_H_
#define FLUTTER_PLUGIN_LYCHEE_TEXTURE_H_

#include <windows.h>

#include <flutter/texture_registrar.h>
#include <lychee/lychee.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace lychee_flutter {

// Bridges one lychee_player to a Flutter pixel buffer texture. A thread
// waits for each DWM composition (the display's vsync) and asks the player
// whether the next vsync shows a new frame; if so, Flutter is told to pull
// it through the pixel buffer callback on its raster thread.
class LycheeTexture {
 public:
  // |window| is the Flutter view, to find the monitor that shows it.
  LycheeTexture(lychee_player* player,
                flutter::TextureRegistrar* registrar,
                HWND window);
  ~LycheeTexture();

  LycheeTexture(const LycheeTexture&) = delete;
  LycheeTexture& operator=(const LycheeTexture&) = delete;

  // Registers the texture and starts the vsync thread. Returns its id.
  int64_t Start();
  // After this returns the player is never touched again and may be
  // destroyed. The texture must still be unregistered; delete it once
  // Flutter says it is unregistered, since the raster thread may still call
  // the pixel buffer callback until then.
  void Dispose();

 private:
  void VsyncLoop();
  const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width,
                                                   size_t height);

  flutter::TextureRegistrar* const registrar_;
  const HWND window_;
  flutter::TextureVariant texture_;
  int64_t texture_id_ = -1;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  // The vsync the last has_new_frame() call was for.
  std::atomic<int64_t> next_display_time_{0};
  std::atomic<bool> frame_pending_{false};

  std::mutex mutex_;
  lychee_player* player_;  // Guarded by |mutex_|.
  // Raster thread only: what the last callback handed to Flutter. The
  // frame stays acquired until Flutter has uploaded it.
  FlutterDesktopPixelBuffer buffer_ = {};
  std::vector<uint8_t> pixels_;
};

}  // namespace lychee_flutter

#endif  // FLUTTER_PLUGIN_LYCHEE_TEXTURE_H_
