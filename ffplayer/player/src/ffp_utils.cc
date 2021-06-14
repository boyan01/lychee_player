//
// Created by yangbin on 2021/2/13.
//

#include "ffp_utils.h"

extern "C" {
#include "libavutil/avutil.h"
}

namespace media {

const char *av_err_to_str(int errnum) {
  static char av_error[AV_ERROR_MAX_STRING_SIZE] = {0};
  return av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum);
}

}