#include "include/lychee_player/lychee_player_plugin.h"

#include <external_media_texture.h>
#include <ffp_flutter.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>

#include <map>
#include <memory>
#include <mutex>

#include "base/logging.h"

namespace {

flutter::TextureRegistrar *texture_registrar_;

class WindowsMediaTexture : public ExternalMediaTexture {
 public:
  WindowsMediaTexture() : pixel_buffer_(new FlutterDesktopPixelBuffer) {
    pixel_buffer_->width = 0;
    pixel_buffer_->height = 0;
    pixel_buffer_->buffer = nullptr;
    pixel_buffer_->release_context = this;
    pixel_buffer_->release_callback = [](void *release_context) {
      auto *texture = reinterpret_cast<WindowsMediaTexture *>(release_context);
      texture->render_mutex_.unlock();
    };
    texture_ =
        std::make_unique<flutter::TextureVariant>(flutter::PixelBufferTexture(
            [&](size_t width,
                size_t height) -> const FlutterDesktopPixelBuffer * {
              if (pixel_buffer_->buffer) {
                render_mutex_.lock();
              }
              return pixel_buffer_;
            }));
    texture_id_ = texture_registrar_->RegisterTexture(texture_.get());
  }

  ~WindowsMediaTexture() override {
    std::lock_guard lck(render_mutex_);

    if (pixel_buffer_->buffer) {
      delete[] pixel_buffer_->buffer;
    }
    delete pixel_buffer_;
    pixel_buffer_ = nullptr;
    // texture_registrar_->UnregisterTexture(texture_id_);
  }

  int64_t GetTextureId() override { return texture_id_; }

  void MaybeInitPixelBuffer(int width, int height) override {
    if (!pixel_buffer_->buffer) {
      DLOG(INFO) << "init pixel buffer: " << width << "x" << height;
      pixel_buffer_->width = width;
      pixel_buffer_->height = height;
      pixel_buffer_->buffer = new uint8_t[width * height * 4];
    }
  }

  int GetWidth() override { return (int)pixel_buffer_->width; }

  int GetHeight() override { return (int)pixel_buffer_->height; }

  PixelFormat GetSupportFormat() override { return kFormat_32_RGBA; }

  bool TryLockBuffer() override { return render_mutex_.try_lock(); }

  void UnlockBuffer() override { render_mutex_.unlock(); }
  void NotifyBufferUpdate() override {
    DCHECK(pixel_buffer_->buffer) << "pixel buffer didn't initialized.";
    texture_registrar_->MarkTextureFrameAvailable(texture_id_);
  }

  uint8_t *GetBuffer() override { return (uint8_t *)pixel_buffer_->buffer; }

 private:
  int64_t texture_id_ = -1;
  FlutterDesktopPixelBuffer *pixel_buffer_;
  std::unique_ptr<flutter::TextureVariant> texture_;

  std::mutex render_mutex_;
};

void flutter_texture_factory(
    std::function<void(std::unique_ptr<ExternalMediaTexture>)> callback) {
  if (!callback) {
    abort();
  }
  if (!texture_registrar_) {
    callback(std::unique_ptr<ExternalMediaTexture>(nullptr));
    return;
  }
  callback(std::make_unique<WindowsMediaTexture>());
}

}  // namespace

void LycheePlayerPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef ref) {
  auto registrar = flutter::PluginRegistrarManager::GetInstance()
                       ->GetRegistrar<flutter::PluginRegistrarWindows>(ref);
  texture_registrar_ = registrar->texture_registrar();
  windows_register_texture(flutter_texture_factory);
}
