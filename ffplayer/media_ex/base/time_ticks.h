//
// Created by yangbin on 2021/4/17.
//

#ifndef MEDIA_MEDIA_EX_BASE_TIME_TICKS_H_
#define MEDIA_MEDIA_EX_BASE_TIME_TICKS_H_

#include "cstdint"

namespace media {

/**
 * Represents monotonically non-decreasing clock time.
 */
class TimeTicks {

 public:

  static TimeTicks Now();

 private:

  constexpr explicit TimeTicks(int64_t us);

  // Time value in a microsecond timebase.
  int64_t us_;

};

}

#endif //MEDIA_MEDIA_EX_BASE_TIME_TICKS_H_
