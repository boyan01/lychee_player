//
// Created by yangbin on 2021/3/6.
//

#ifndef MEDIA_SDL_SDL_UTILS_H_
#define MEDIA_SDL_SDL_UTILS_H_

extern "C" {
#include "SDL2/SDL.h"
#include "libavutil/rational.h"
}

namespace media {

namespace sdl {

void calculate_display_rect(SDL_Rect *rect, int scr_xleft, int scr_ytop,
                            int scr_width, int scr_height, int pic_width,
                            int pic_height, AVRational pic_sar);

}

} // namespace media

#endif // MEDIA_SDL_SDL_UTILS_H_
