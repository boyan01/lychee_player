//
// Created by yangbin on 2021/4/17.
//

#include "base/time_delta.h"

namespace media {

constexpr int TimeDelta::InHours() const {
  // saturated_cast<> is necessary since very large (but still less than
  // min/max) deltas would result in overflow.
  return static_cast<int>(delta_ / kMicrosecondsPerHour);
}

constexpr int TimeDelta::InMinutes() const {
  // saturated_cast<> is necessary since very large (but still less than
  // min/max) deltas would result in overflow.
  return static_cast<int>(delta_ / kMicrosecondsPerMinute);
}

constexpr int64_t TimeDelta::InSeconds() const {
  return is_inf() ? delta_ : (delta_ / kMicrosecondsPerSecond);
}

constexpr int64_t TimeDelta::InNanoseconds() const {
  return delta_ * kNanosecondsPerMicrosecond;
}

int64_t TimeDelta::InMilliseconds() const {
  if (!is_inf())
    return delta_ / kMicrosecondsPerMillisecond;
  return (delta_ < 0) ? std::numeric_limits<int64_t>::min()
                      : std::numeric_limits<int64_t>::max();
}

int64_t TimeDelta::InMillisecondsRoundedUp() const {
  if (!is_inf()) {
    const int64_t result = delta_ / kMicrosecondsPerMillisecond;
    // Convert |result| from truncating to ceiling.
    return (delta_ > result * kMicrosecondsPerMillisecond) ? (result + 1) : result;
  }
  return delta_;
}

std::ostream &operator<<(std::ostream &os, const TimeDelta &delta) {
  os << delta.InMilliseconds();
  return os;
}

}