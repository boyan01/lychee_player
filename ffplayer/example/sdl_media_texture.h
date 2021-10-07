//
// Created by yangbin on 2021/10/7.
//

#ifndef MEDIA_EXAMPLE_SDL_MEDIA_TEXTURE_H_
#define MEDIA_EXAMPLE_SDL_MEDIA_TEXTURE_H_

#include "external_media_texture.h"

extern "C" {
#include "SDL2/SDL.h"
}

class SdlMediaTexture : public ExternalMediaTexture {
 public:

  SdlMediaTexture();

  ~SdlMediaTexture() override;

  int64_t GetTextureId() override;

  void MaybeInitPixelBuffer(int width, int height) override;

  int GetWidth() override;

  int GetHeight() override;

  PixelFormat GetSupportFormat() override;

  bool TryLockBuffer() override;

  void UnlockBuffer() override;

  void NotifyBufferUpdate() override;

  uint8_t *GetBuffer() override;

 private:
  std::shared_ptr<SDL_Renderer> renderer_;

  SDL_Texture *texture_ = nullptr;

  // texture_ locked pixel and pitch.
  void *texture_pixel_;

  int texture_pitch_;

};

#endif //MEDIA_EXAMPLE_SDL_MEDIA_TEXTURE_H_
