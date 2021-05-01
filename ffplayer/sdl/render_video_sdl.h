//
// Created by boyan on 2021/2/16.
//

#ifndef FFP_SRC_RENDER_VIDEO_SDL_H_
#define FFP_SRC_RENDER_VIDEO_SDL_H_

#include "render_video_base.h"

extern "C" {
#include "SDL2/SDL.h"
#include "libswscale/swscale.h"
};

namespace media {

class SdlVideoRender : public VideoRenderBase {

 public:
  int screen_width = 0;
  int screen_height = 0;

 private:
  std::shared_ptr<SDL_Renderer> renderer_;
  SDL_Texture *texture_ = nullptr;
  SDL_Texture *sub_texture_ = nullptr;
  struct SwsContext *img_convert_ctx = nullptr;

 private:

  int ReAllocTexture(Uint32 new_format, int new_width, int new_height,
                     SDL_BlendMode blendmode, int init_texture);

  int UploadTexture(AVFrame *frame);

  static void GetSdlPixFmtAndBlendMode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode);

  static void SetSdlYuvConversionMode(AVFrame *frame);

 public:

  explicit SdlVideoRender(std::shared_ptr<SDL_Renderer> renderer);

  ~SdlVideoRender() override;

  void RenderPicture(std::shared_ptr<VideoFrame> frame) override;

  void DestroyTexture();
};

}
#endif //FFP_SRC_RENDER_VIDEO_SDL_H_
