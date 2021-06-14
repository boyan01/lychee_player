//
// Created by yangbin on 2021/4/17.
//

#ifndef MEDIA_BASE_TIME_TICKS_H_
#define MEDIA_BASE_TIME_TICKS_H_

#include "cstdint"

#include "base/time_delta.h"

namespace media {

/**
 * Represents monotonically non-decreasing clock time.
 */
class TimeTicks {

 public:

  static TimeTicks Now();

  TimeTicks operator+(const TimeDelta &time_delta) const;

  TimeDelta operator-(const TimeTicks &time_ticks) const;

  bool operator<(const TimeTicks &rhs) const;
  bool operator>(const TimeTicks &rhs) const;
  bool operator<=(const TimeTicks &rhs) const;
  bool operator>=(const TimeTicks &rhs) const;

  bool operator==(const TimeTicks &rhs) const;
  bool operator!=(const TimeTicks &rhs) const;

 private:

  constexpr explicit TimeTicks(int64_t us);

  // Time value in a microsecond timebase.
  int64_t us_;

};

}

#endif //MEDIA_BASE_TIME_TICKS_H_
