#ifndef FFPLAYER_UTILS_H_
#define FFPLAYER_UTILS_H_

extern "C" {
#include "libavutil/rational.h"
#include "libavutil/error.h"
}

void update_thread_name(const char *name);

const char *av_err_to_str(int errnum);

#endif  // FFPLAYER_UTILS_H_