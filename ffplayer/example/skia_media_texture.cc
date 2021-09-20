//
// Created by Bin Yang on 2021/9/20.
//

#include "map"

#include "skia_media_texture.h"

namespace {

std::map<int64_t, os::Ref<os::SkiaSurface>> textures;

void registerFactory(std::function<void(std::unique_ptr<ExternalMediaTexture>)> callback) { // NOLINT(performance-unnecessary-value-param)
  static int64_t texture_id = 0;
  auto surface = os::make_ref<os::SkiaSurface>();
  textures.insert(std::pair(texture_id, surface));
  auto texture = std::make_unique<SkiaMediaTexture>(surface, texture_id);
  callback(std::move(texture));
  texture_id++;
}

}

void SkiaMediaTexture::RegisterTextureFactory() {
  register_external_texture_factory(registerFactory);
}

const os::Ref<os::SkiaSurface> &SkiaMediaTexture::GetExternalSurface(int64_t texture_id) {
  return textures.at(texture_id);
}

SkiaMediaTexture::SkiaMediaTexture(os::Ref<os::SkiaSurface> surface, int64_t texture_id)
    : surface_(std::move(surface)),
      texture_id_(texture_id) {
}

int64_t SkiaMediaTexture::GetTextureId() {
  return texture_id_;
}

void SkiaMediaTexture::MaybeInitPixelBuffer(int width, int height) {
  surface_->createRgba(width, height, nullptr);
}

int SkiaMediaTexture::GetWidth() {
  return surface_->width();
}

int SkiaMediaTexture::GetHeight() {
  return surface_->height();
}

ExternalMediaTexture::PixelFormat SkiaMediaTexture::GetSupportFormat() {
  return kFormat_32_RGBA;
}

void SkiaMediaTexture::UnlockBuffer() {
  surface_->unlock();
}

bool SkiaMediaTexture::TryLockBuffer() {
  if (surface_->bitmap().drawsNothing()) {
    return false;
  }
  surface_->lock();
  return true;
}

void SkiaMediaTexture::NotifyBufferUpdate() {
  surface_->flush();
}

uint8_t *SkiaMediaTexture::GetBuffer() {
  return static_cast<uint8_t *>(surface_->bitmap().getPixels());
}
