//
// Created by yangbin on 2021/2/13.
//

#include "ffp_utils.h"

#if WIN32

#include <windows.h>
#include <processthreadsapi.h>
#include <string>

#else
#include "pthread.h"
#endif

extern "C" {
#include "libavutil/avutil.h"
}

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

void update_thread_name(const char *name) {
#if __LINUX__
  pthread_setname_np(pthread_self(), name);
#elif WIN32
  int ret = MultiByteToWideChar(CP_UTF8, 0, name, strlen(name), nullptr, 0);
  if (ret < 0) {
    return;
  }
  std::wstring wide_name;
  wide_name.resize(ret + 10);

  if (MultiByteToWideChar(CP_UTF8, 0, name, (int) strlen(name), &wide_name[0], (int) wide_name.size()) < 0) {
    return;
  }
  SetThreadDescription(GetCurrentThread(), wide_name.data());
#else
  pthread_setname_np(name);
#endif

}
