//
// Created by yangbin on 2021/10/7.
//
#include <sdl_utils.h>
#include "base/logging.h"

#include "sdl_media_texture.h"
#include "app_window.h"

SdlMediaTexture::SdlMediaTexture() :
    renderer_(AppWindow::Instance()->GetRenderer()),
    texture_pixel_(),
    texture_pitch_() {
  DCHECK(renderer_);
}

SdlMediaTexture::~SdlMediaTexture() {
  SDL_DestroyTexture(texture_);
  DCHECK(!texture_pixel_);
}

int64_t SdlMediaTexture::GetTextureId() {
  return 0;
}

void SdlMediaTexture::MaybeInitPixelBuffer(int width, int height) {
  if (texture_) {
    return;
  }
  texture_ =
      SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, width, height);
  DCHECK(texture_) << "failed to create texture: " << SDL_GetError();
  auto ret = SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
  DCHECK_EQ(ret, 0) << "unable set blend mode: " << SDL_GetError();

  void *pixels;
  int pitch;
  if (SDL_LockTexture(texture_, nullptr, &pixels, &pitch) < 0) {
    NOTREACHED();
  }
  SDL_memset(pixels, 0, pitch * height);
  SDL_UnlockTexture(texture_);

}

int SdlMediaTexture::GetWidth() {
  int width;
  SDL_QueryTexture(texture_, nullptr, nullptr, &width, nullptr);
  return width;
}

int SdlMediaTexture::GetHeight() {
  int height;
  SDL_QueryTexture(texture_, nullptr, nullptr, nullptr, &height);
  return height;
}

ExternalMediaTexture::PixelFormat SdlMediaTexture::GetSupportFormat() {
  return kFormat_32_BGRA;
}

bool SdlMediaTexture::TryLockBuffer() {
  return !SDL_LockTexture(texture_, nullptr, &texture_pixel_, &texture_pitch_);
}

void SdlMediaTexture::UnlockBuffer() {
  SDL_UnlockTexture(texture_);
  texture_pixel_ = nullptr;
}

void SdlMediaTexture::NotifyBufferUpdate() {
  SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, 255);
  SDL_RenderClear(renderer_.get());
  // TODO calculate center rect.
  SDL_Rect rect{};

  int frame_width;
  int frame_height;
  SDL_QueryTexture(texture_, nullptr, nullptr, &frame_width, &frame_height);

  int window_width;
  int window_height;
  SDL_GetWindowSize(AppWindow::Instance()->GetWindow(), &window_width, &window_height);

  media::sdl::calculate_display_rect(&rect,
                                     0,
                                     0,
                                     window_width,
                                     window_height,
                                     frame_width,
                                     frame_height,
                                     // TODO SAR
                                     AVRational{1, 1});
  SDL_RenderCopyEx(renderer_.get(), texture_, nullptr, &rect, 0, nullptr, SDL_FLIP_NONE);
  SDL_RenderPresent(renderer_.get());
}

uint8_t *SdlMediaTexture::GetBuffer() {
  DCHECK(texture_pixel_) << "texture is not locking.";
  return reinterpret_cast<uint8_t *>(texture_pixel_);
}

