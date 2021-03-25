#ifndef FFPLAYER_UTILS_H_
#define FFPLAYER_UTILS_H_

extern "C" {
#include "libavutil/rational.h"
#include "libavutil/error.h"
#include "libavutil/time.h" // NOLINT(modernize-deprecated-headers)
}

namespace media {

void update_thread_name(const char *name);

const char *av_err_to_str(int errnum);

static inline double get_relative_time() {
  return av_gettime_relative() / 1000000.0;
}

}
#endif  // FFPLAYER_UTILS_H_