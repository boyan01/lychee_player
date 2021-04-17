//
// Created by yangbin on 2021/4/17.
//

#include "time_ticks.h"

extern "C" {
#include "libavutil/time.h" // NOLINT(modernize-deprecated-headers)
}

namespace media {

constexpr TimeTicks::TimeTicks(int64_t us) : us_(us) {}

TimeTicks TimeTicks::Now() {
  int64_t us = av_gettime_relative();
  return TimeTicks(us);
}

}
