#ifndef FFPLAYER_UTILS_H_
#define FFPLAYER_UTILS_H_

extern "C" {
#include "libavutil/rational.h"
#include "SDL2/SDL_rect.h"
}

void update_thread_name(const char *name);

void calculate_display_rect(SDL_Rect *rect,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar);

#endif  // FFPLAYER_UTILS_H_