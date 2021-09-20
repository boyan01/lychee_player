//
// Created by Bin Yang on 2021/9/20.
//

#ifndef MEDIA_EXAMPLE_SKIA_MEDIA_TEXTURE_H_
#define MEDIA_EXAMPLE_SKIA_MEDIA_TEXTURE_H_

#include "os/skia/skia_surface.h"
#include "SkBitmap.h"

#include "external_media_texture.h"

class SkiaMediaTexture : public ExternalMediaTexture {
 public:
  static void RegisterTextureFactory();

  static const os::Ref<os::SkiaSurface> &GetExternalSurface(int64_t texture_id);

  SkiaMediaTexture(os::Ref<os::SkiaSurface> surface, int64_t texture_id);

  int64_t GetTextureId() override;
  void MaybeInitPixelBuffer(int width, int height) override;
  int GetWidth() override;
  int GetHeight() override;
  PixelFormat GetSupportFormat() override;
  void UnlockBuffer() override;
  bool TryLockBuffer() override;
  void NotifyBufferUpdate() override;
  uint8_t *GetBuffer() override;

 private:

  const SkBitmap &bitmap() { return surface_->bitmap(); }

  os::Ref<os::SkiaSurface> surface_;
  int64_t texture_id_;

};

#endif //MEDIA_EXAMPLE_SKIA_MEDIA_TEXTURE_H_
