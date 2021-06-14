//
// Created by yangbin on 2021/4/17.
//

#include "base/time_ticks.h"

#include <chrono>

namespace media {

constexpr TimeTicks::TimeTicks(int64_t us) : us_(us) {}

TimeTicks TimeTicks::Now() {
  auto time = std::chrono::system_clock::now().time_since_epoch();
  auto us = std::chrono::duration_cast<std::chrono::microseconds>(time);
  return TimeTicks(us.count());
}

bool TimeTicks::operator==(const TimeTicks &rhs) const {
  return us_ == rhs.us_;
}

bool TimeTicks::operator!=(const TimeTicks &rhs) const {
  return us_ != rhs.us_;
}

bool TimeTicks::operator<(const TimeTicks &rhs) const {
  return us_ < rhs.us_;
}

bool TimeTicks::operator>(const TimeTicks &rhs) const {
  return us_ > rhs.us_;
}

bool TimeTicks::operator<=(const TimeTicks &rhs) const {
  return us_ <= rhs.us_;
}

bool TimeTicks::operator>=(const TimeTicks &rhs) const {
  return us_ >= rhs.us_;
}

TimeTicks TimeTicks::operator+(const TimeDelta &time_delta) const {
  return TimeTicks(us_ + time_delta.InMicroseconds());
}

TimeDelta TimeTicks::operator-(const TimeTicks &time_ticks) const {
  return TimeDelta::FromMicroseconds(us_ - time_ticks.us_);
}

}
