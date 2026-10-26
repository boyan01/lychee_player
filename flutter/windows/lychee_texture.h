#ifndef FLUTTER_PLUGIN_LYCHEE_TEXTURE_H_
#define FLUTTER_PLUGIN_LYCHEE_TEXTURE_H_

#include <windows.h>

#include <d3d11.h>
#include <flutter/texture_registrar.h>
#include <lychee/lychee.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lychee_flutter {

// Bridges one lychee_player to a Flutter texture. A thread waits for each
// DWM composition (the display's vsync) and asks the player whether the
// next vsync shows a new frame; if so, Flutter is told to pull it through
// the texture callback on its raster thread.
//
// With the adapter Flutter renders on, the texture is a GPU surface:
// hardware-decoded frames are shared D3D11 textures on that adapter, and
// software-decoded BGRA frames are uploaded into one. Without it, a pixel
// buffer texture that Flutter uploads itself.
class LycheeTexture {
 public:
  // |window| is the Flutter view, to find the monitor that shows it.
  // |adapter| is Flutter's, or null.
  LycheeTexture(lychee_player* player,
                flutter::TextureRegistrar* registrar,
                HWND window,
                IDXGIAdapter* adapter);
  ~LycheeTexture();

  LycheeTexture(const LycheeTexture&) = delete;
  LycheeTexture& operator=(const LycheeTexture&) = delete;

  // Registers the texture and starts the vsync thread. Returns its id.
  int64_t Start();
  // After this returns the player is never touched again and may be
  // destroyed. The texture must still be unregistered; delete it once
  // Flutter says it is unregistered, since the raster thread may still call
  // the texture callback until then.
  void Dispose();

  // The player it was created for, even after Dispose(): the plugin
  // destroys it when Dart, which owns it, is gone.
  lychee_player* player() const { return created_player_; }

 private:
  void VsyncLoop();
  // Acquires the frame for the next vsync. Raster thread.
  lychee_frame* AcquireFrame();
  const FlutterDesktopPixelBuffer* CopyPixelBuffer(size_t width,
                                                   size_t height);
  const FlutterDesktopGpuSurfaceDescriptor* CopyGpuSurface(size_t width,
                                                           size_t height);
  // A shared texture holding |frame|'s pixels, or null.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> Upload(lychee_frame* frame);

  flutter::TextureRegistrar* const registrar_;
  const HWND window_;
  lychee_player* const created_player_;
  // For uploads; null without an adapter.
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11Query> completion_;
  std::unique_ptr<flutter::TextureVariant> texture_;
  int64_t texture_id_ = -1;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  // The vsync the last has_new_frame() call was for.
  std::atomic<int64_t> next_display_time_{0};
  std::atomic<bool> frame_pending_{false};

  std::mutex mutex_;
  lychee_player* player_;  // Guarded by |mutex_|.

  // Raster thread only: what the last callback handed to Flutter.
  FlutterDesktopPixelBuffer buffer_ = {};
  std::vector<uint8_t> pixels_;
  // Flutter reads the descriptor after the callback returns; the frame and
  // texture behind it stay until the next frame replaces them, since
  // Flutter's release callback only means "imported", not "drawn".
  FlutterDesktopGpuSurfaceDescriptor descriptor_ = {};
  lychee_frame* shown_frame_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> shown_texture_;
};

}  // namespace lychee_flutter

#endif  // FLUTTER_PLUGIN_LYCHEE_TEXTURE_H_
