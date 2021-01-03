#ifndef FFPLAYER_UTILS_H_
#define FFPLAYER_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "libavutil/rational.h"

typedef struct FFP_Rect {
    int x, y;
    int w, h;
} FFP_Rect;

void calculate_display_rect(FFP_Rect *rect,
                            int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                            int pic_width, int pic_height, AVRational pic_sar);

#ifdef __cplusplus
}
#endif

#endif  // FFPLAYER_UTILS_H_