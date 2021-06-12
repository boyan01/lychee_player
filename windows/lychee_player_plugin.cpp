#include "include/lychee_player/lychee_player_plugin.h"

#include <windows.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include "base/logging.h"

#include <media_api.h>

#include <map>
#include <memory>
#include <mutex>

namespace {

flutter::TextureRegistrar *texture_registrar_;

class WindowsMediaTexture : public FlutterMediaTexture {

 public:

  WindowsMediaTexture() : pixel_buffer_(new FlutterDesktopPixelBuffer) {
    texture_ = std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
        [&](size_t width, size_t height) -> const FlutterDesktopPixelBuffer * {
          return pixel_buffer_;
        }));
    texture_id_ = texture_registrar_->RegisterTexture(texture_.get());
    pixel_buffer_->width = 0;
    pixel_buffer_->height = 0;
    pixel_buffer_->buffer = nullptr;
  }

  ~WindowsMediaTexture() override {
    std::lock_guard<std::mutex> lock(render_mutex_);
    texture_.reset(nullptr);
    DLOG(INFO) << "UnregisterTexture: " << texture_id_;
    texture_registrar_->UnregisterTexture(texture_id_);
    delete[] pixel_buffer_->buffer;
    delete pixel_buffer_;
  }

  int64_t GetTextureId() override {
    return texture_id_;
  }

  void MaybeInitPixelBuffer(int width, int height) override {
    if (!pixel_buffer_->buffer) {
      pixel_buffer_->width = width;
      pixel_buffer_->height = height;
      pixel_buffer_->buffer = new uint8_t[width * height * 4];
    }
  }

  int GetWidth() override {
    return (int) pixel_buffer_->width;
  }

  int GetHeight() override {
    return (int) pixel_buffer_->height;
  }

  PixelFormat GetSupportFormat() override {
    return kFormat_32_RGBA;
  }

  bool TryLockBuffer() override {
    return render_mutex_.try_lock();
  }

  void UnlockBuffer() override {
    render_mutex_.unlock();
  }
  void NotifyBufferUpdate() override {
    texture_registrar_->MarkTextureFrameAvailable(texture_id_);
  }

  uint8_t *GetBuffer() override {
    return (uint8_t *) pixel_buffer_->buffer;
  }

 private:
  int64_t texture_id_ = -1;
  FlutterDesktopPixelBuffer *pixel_buffer_;
  std::unique_ptr<flutter::TextureVariant> texture_;

  std::mutex render_mutex_;

};

void flutter_texture_factory(std::function<void(std::unique_ptr<FlutterMediaTexture>)> callback) {
  if (!callback) {
    abort();
  }
  if (!texture_registrar_) {
    callback(std::unique_ptr<FlutterMediaTexture>(nullptr));
    return;
  }
  callback(std::make_unique<WindowsMediaTexture>());
}

} // namespace

void LycheePlayerPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef ref) {
  auto registrar = flutter::PluginRegistrarManager::GetInstance()->GetRegistrar<flutter::PluginRegistrarWindows>(ref);
  texture_registrar_ = registrar->texture_registrar();
  register_flutter_texture_factory(flutter_texture_factory);
}
