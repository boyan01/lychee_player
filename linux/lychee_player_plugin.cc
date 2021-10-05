#include "include/lychee_player/lychee_player_plugin.h"
#include "include/lychee_player/fl_media_texture.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <base/logging.h>
#include <external_media_texture.h>

namespace {

FlTextureRegistrar *fl_texture_registrar;

class FlExternalMediaTextureImpl : public ExternalMediaTexture {
 public:

  FlExternalMediaTextureImpl() : media_texture_(fl_media_texture_new()) {
    DCHECK(fl_texture_registrar);
    auto success = fl_texture_registrar_register_texture(fl_texture_registrar, FL_TEXTURE(media_texture_));
    DCHECK(success) << "fl_texture_registrar_register_texture failed.";
    if (!success) {
      g_object_unref(media_texture_);
      media_texture_ = nullptr;
    } else {
      media_texture_->texture_id = reinterpret_cast<int64_t>(FL_TEXTURE(media_texture_));
    }
  }

  ~FlExternalMediaTextureImpl() override {
    if (media_texture_) {
      fl_texture_registrar_unregister_texture(fl_texture_registrar, FL_TEXTURE(media_texture_));
      delete[] media_texture_->buffer;
      g_object_unref(media_texture_);

    }
  }

  int64_t GetTextureId() override {
    return media_texture_ ? media_texture_->texture_id : -1;
  }

  void MaybeInitPixelBuffer(int width, int height) override {
    if (media_texture_) {
      media_texture_->buffer = new uint8_t[width * height * 4];
      media_texture_->width = width;
      media_texture_->height = height;
    }
  }
  int GetWidth() override {
    return media_texture_ ? media_texture_->height : 0;
  }
  int GetHeight() override {
    return media_texture_ ? media_texture_->width : 0;
  }
  PixelFormat GetSupportFormat() override {
    return kFormat_32_RGBA;
  }
  void UnlockBuffer() override {

  }

  bool TryLockBuffer() override {
    if (!media_texture_) {
      return false;
    }
    return true;
  }

  void NotifyBufferUpdate() override {
    fl_texture_registrar_mark_texture_frame_available(fl_texture_registrar, FL_TEXTURE(media_texture_));
  }

  uint8_t *GetBuffer() override {
    DCHECK(media_texture_);
    return media_texture_->buffer;
  }

 private:
  FlMediaTexture *media_texture_;

};

void fl_external_texture_factory(
    std::function<void(std::unique_ptr<ExternalMediaTexture>)> callback) {

  if (!fl_texture_registrar || !callback) {
    callback(std::unique_ptr<ExternalMediaTexture>(nullptr));
    return;
  }
  callback(std::make_unique<FlExternalMediaTextureImpl>());
}

}

void lychee_player_plugin_register_with_registrar(FlPluginRegistrar *registrar) {
  fl_texture_registrar = fl_plugin_registrar_get_texture_registrar(registrar);
  register_external_texture_factory(fl_external_texture_factory);
}
