//
// Created by yangbin on 2021/5/5.
//

#ifndef MEDIA_SDL_SDL_VIDEO_RENDERER_SINK_H_
#define MEDIA_SDL_SDL_VIDEO_RENDERER_SINK_H_

extern "C" {
#include "SDL2/SDL.h"
#include "libswscale/swscale.h"
}

#include "video_renderer_sink.h"
#include "base/task_runner.h"

namespace media {

class SdlVideoRendererSink : public VideoRendererSink {

 public:

  static int screen_width;
  static int screen_height;

  SdlVideoRendererSink(const TaskRunner &render_task_runner, std::shared_ptr<SDL_Renderer> renderer);

  void Start(RenderCallback *callback) override;

  void Stop() override;

 private:

  enum State {
    kIdle,
    kRunning,
  };

  TaskRunner render_task_runner_;

  std::shared_ptr<SDL_Renderer> renderer_;

  State state_ = kIdle;

  RenderCallback *render_callback_ = nullptr;

  SDL_Texture *texture_ = nullptr;

  struct SwsContext *img_convert_ctx = nullptr;

  void RenderInternal();

  void RenderPicture(const std::shared_ptr<VideoFrame>& frame);

  int ReAllocTexture(Uint32 new_format, int new_width, int new_height,
                     SDL_BlendMode blendmode, int init_texture);

  int UploadTexture(AVFrame *frame);

  static void GetSdlPixFmtAndBlendMode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode);

  static void SetSdlYuvConversionMode(AVFrame *frame);

  DISALLOW_COPY_AND_ASSIGN(SdlVideoRendererSink);

};

} // namespace media

#endif //MEDIA_SDL_SDL_VIDEO_RENDERER_SINK_H_
