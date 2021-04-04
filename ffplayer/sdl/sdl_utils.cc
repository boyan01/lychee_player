//
// Created by yangbin on 2021/3/6.
//

#include "sdl_utils.h"

extern "C" {
#include "libavutil/avutil.h"
#include "SDL2/SDL.h"
}

namespace media {

namespace sdl {

void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop, int scr_width, int scr_height, int pic_width,
                            int pic_height, AVRational pic_sar) {
  AVRational aspect_ratio = pic_sar;
  int64_t width, height, x, y;

  if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
    aspect_ratio = av_make_q(1, 1);

  aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

  /* XXX: we suppose the screen has a 1.0 pixel ratio */
  height = scr_height;
  width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
  if (width > scr_width) {
    width = scr_width;
    height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
  }
  x = (scr_width - width) / 2;
  y = (scr_height - height) / 2;
  rect->x = (int) (scr_xleft + x);
  rect->y = (int) (scr_ytop + y);
  rect->w = FFMAX((int) width, 1);
  rect->h = FFMAX((int) height, 1);
}

void InitSdlAudio() {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
    av_log(nullptr, AV_LOG_ERROR, "SDL fails to initialize audio subsystem!\n%s", SDL_GetError());
  else
    av_log(nullptr, AV_LOG_DEBUG, "SDL Audio was initialized fine!\n");
}

} // namespace sdl
} // namespace media