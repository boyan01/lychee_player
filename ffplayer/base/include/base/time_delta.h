//
// Created by yangbin on 2021/4/17.
//

#ifndef MEDIA_BASE_TIME_DELTA_H_
#define MEDIA_BASE_TIME_DELTA_H_

#include <cstdint>
#include <limits>
#include <ostream>

#include "base/logging.h"

namespace media {

static constexpr int64_t kHoursPerDay = 24;
static constexpr int64_t kSecondsPerMinute = 60;
static constexpr int64_t kMinutesPerHour = 60;
static constexpr int64_t kSecondsPerHour = kSecondsPerMinute * kMinutesPerHour;
static constexpr int64_t kMillisecondsPerSecond = 1000;
static constexpr int64_t kMillisecondsPerDay =
    kMillisecondsPerSecond * kSecondsPerHour * kHoursPerDay;
static constexpr int64_t kMicrosecondsPerMillisecond = 1000;
static constexpr int64_t kMicrosecondsPerSecond =
    kMicrosecondsPerMillisecond * kMillisecondsPerSecond;
static constexpr int64_t kMicrosecondsPerMinute =
    kMicrosecondsPerSecond * kSecondsPerMinute;
static constexpr int64_t kMicrosecondsPerHour =
    kMicrosecondsPerMinute * kMinutesPerHour;
static constexpr int64_t kMicrosecondsPerDay =
    kMicrosecondsPerHour * kHoursPerDay;
static constexpr int64_t kMicrosecondsPerWeek = kMicrosecondsPerDay * 7;
static constexpr int64_t kNanosecondsPerMicrosecond = 1000;
static constexpr int64_t kNanosecondsPerSecond =
    kNanosecondsPerMicrosecond * kMicrosecondsPerSecond;

class TimeDelta {
 public:
  constexpr TimeDelta() = default;

  // Converts units of time to TimeDeltas.
  // These conversions treat minimum argument values as min type values or -inf,
  // and maximum ones as max type values or +inf; and their results will produce
  // an is_min() or is_max() TimeDelta. WARNING: Floating point arithmetic is
  // such that FromXXXD(t.InXXXF()) may not precisely equal |t|. Hence, floating
  // point values should not be used for storage.
  static constexpr TimeDelta FromDays(int days);
  static constexpr TimeDelta FromHours(int hours);
  static constexpr TimeDelta FromMinutes(int minutes);
  static constexpr TimeDelta FromSecondsD(double secs);
  static constexpr TimeDelta FromSeconds(int64_t secs);
  static constexpr TimeDelta FromMillisecondsD(double ms);
  static constexpr TimeDelta FromMilliseconds(int64_t ms);
  static constexpr TimeDelta FromMicrosecondsD(double us);
  static constexpr TimeDelta FromMicroseconds(int64_t us);
  static constexpr TimeDelta FromNanosecondsD(double ns);
  static constexpr TimeDelta FromNanoseconds(int64_t ns);

  static constexpr TimeDelta Zero() { return TimeDelta(0); }

  // Returns the maximum time delta, which should be greater than any reasonable
  // time delta we might compare it to. Adding or subtracting the maximum time
  // delta to a time or another time delta has an undefined result.
  static constexpr TimeDelta Max();

  // Returns the minimum time delta, which should be less than than any
  // reasonable time delta we might compare it to. Adding or subtracting the
  // minimum time delta to a time or another time delta has an undefined result.
  static constexpr TimeDelta Min();

  // Returns true if the time delta is zero.
  constexpr bool is_zero() const { return delta_ == 0; }

  // Returns true if the time delta is the maximum/minimum time delta.
  constexpr bool is_max() const { return *this == Max(); }
  constexpr bool is_min() const { return *this == Min(); }
  constexpr bool is_inf() const { return is_min() || is_max(); }

  constexpr bool is_positive() const { return delta_ > 0; }

  constexpr int InHours() const;
  constexpr int InMinutes() const;
  constexpr double InSecondsF() const;
  constexpr int64_t InSeconds() const;

  int64_t InMilliseconds() const;

  int64_t InMillisecondsRoundedUp() const;

  constexpr int64_t InMicroseconds() const { return delta_; }

  constexpr int64_t InNanoseconds() const;

  // Computations with other deltas.
  constexpr TimeDelta operator+(TimeDelta other) const;

  constexpr TimeDelta operator-(TimeDelta other) const;

  constexpr TimeDelta &operator+=(TimeDelta other) {
    return *this = (*this + other);
  }
  constexpr TimeDelta &operator-=(TimeDelta other) {
    return *this = (*this - other);
  }
  constexpr TimeDelta operator-() const {
    if (!is_inf()) return TimeDelta(-delta_);
    return (delta_ < 0) ? Max() : Min();
  }

  // Computations with numeric types.
  template <typename T>
  constexpr TimeDelta operator*(T a) const {
    return TimeDelta(delta_ * a);
  }

  template <typename T>
  constexpr TimeDelta operator/(T a) const {
    return TimeDelta(delta_ / a);
  }

  template <typename T>
  constexpr TimeDelta &operator*=(T a) {
    return *this = (*this * a);
  }
  template <typename T>
  constexpr TimeDelta &operator/=(T a) {
    return *this = (*this / a);
  }

  // This does floating-point division. For an integer result, either call
  // IntDiv(), or (possibly clearer) use this operator with
  // base::Clamp{Ceil,Floor,Round}() or base::saturated_cast() (for truncation).
  // Note that converting to double here drops precision to 53 bits.
  constexpr double operator/(TimeDelta a) const {
    // 0/0 and inf/inf (any combination of positive and negative) are invalid
    // (they are almost certainly not intentional, and result in NaN, which
    // turns into 0 if clamped to an integer; this makes introducing subtle bugs
    // too easy).
    CHECK(!is_zero() || !a.is_zero());
    CHECK(!is_inf() || !a.is_inf());

    return ToDouble() / a.ToDouble();
  }
  constexpr int64_t IntDiv(TimeDelta a) const {
    if (!is_inf() && !a.is_zero()) return delta_ / a.delta_;

    // For consistency, use the same edge case CHECKs and behavior as the code
    // above.
    CHECK(!is_zero() || !a.is_zero());
    CHECK(!is_inf() || !a.is_inf());
    return ((delta_ < 0) == (a.delta_ < 0))
               ? std::numeric_limits<int64_t>::max()
               : std::numeric_limits<int64_t>::min();
  }

  constexpr TimeDelta operator%(TimeDelta a) const {
    return TimeDelta(
        (is_inf() || a.is_zero() || a.is_inf()) ? delta_ : (delta_ % a.delta_));
  }
  TimeDelta &operator%=(TimeDelta other) { return *this = (*this % other); }

  // Comparison operators.
  constexpr bool operator==(TimeDelta other) const {
    return delta_ == other.delta_;
  }
  constexpr bool operator!=(TimeDelta other) const {
    return delta_ != other.delta_;
  }
  constexpr bool operator<(TimeDelta other) const {
    return delta_ < other.delta_;
  }
  constexpr bool operator<=(TimeDelta other) const {
    return delta_ <= other.delta_;
  }
  constexpr bool operator>(TimeDelta other) const {
    return delta_ > other.delta_;
  }
  constexpr bool operator>=(TimeDelta other) const {
    return delta_ >= other.delta_;
  }

  friend std::ostream &operator<<(std::ostream &os, const TimeDelta &delta);

 private:
  constexpr explicit TimeDelta(int64_t delta_us) : delta_(delta_us) {}

  // Returns a double representation of this TimeDelta's tick count.  In
  // particular, Max()/Min() are converted to +/-infinity.
  constexpr double ToDouble() const {
    if (!is_inf()) return static_cast<double>(delta_);
    return (delta_ < 0) ? -std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::infinity();
  }

  // Delta in microseconds.
  int64_t delta_ = 0;
};

// TimeDelta functions that must appear below the declarations of Time/TimeDelta

// static
constexpr TimeDelta TimeDelta::FromDays(int days) {
  return (days == std::numeric_limits<int>::max())
             ? Max()
             : TimeDelta(days * kMicrosecondsPerDay);
}

// static
constexpr TimeDelta TimeDelta::FromHours(int hours) {
  return (hours == std::numeric_limits<int>::max())
             ? Max()
             : TimeDelta(hours * kMicrosecondsPerHour);
}

// static
constexpr TimeDelta TimeDelta::FromMinutes(int minutes) {
  return (minutes == std::numeric_limits<int>::max())
             ? Max()
             : TimeDelta(minutes * kMicrosecondsPerMinute);
}

// static
constexpr TimeDelta TimeDelta::FromSecondsD(double secs) {
  return TimeDelta(static_cast<int64_t>(secs * kMicrosecondsPerSecond));
}

// static
constexpr TimeDelta TimeDelta::FromSeconds(int64_t secs) {
  return TimeDelta(int64_t{(secs * kMicrosecondsPerSecond)});
}

// static
constexpr TimeDelta TimeDelta::FromMillisecondsD(double ms) {
  return TimeDelta(static_cast<int64_t>(ms * kMicrosecondsPerMillisecond));
}

// static
constexpr TimeDelta TimeDelta::FromMilliseconds(int64_t ms) {
  return TimeDelta(int64_t{(ms * kMicrosecondsPerMillisecond)});
}

// static
constexpr TimeDelta TimeDelta::FromMicrosecondsD(double us) {
  return TimeDelta(static_cast<int64_t>(us));
}

// static
constexpr TimeDelta TimeDelta::FromMicroseconds(int64_t us) {
  return TimeDelta(us);
}

// static
constexpr TimeDelta TimeDelta::FromNanosecondsD(double ns) {
  return TimeDelta(static_cast<int64_t>(ns / kNanosecondsPerMicrosecond));
}

// static
constexpr TimeDelta TimeDelta::FromNanoseconds(int64_t ns) {
  return TimeDelta(ns / kNanosecondsPerMicrosecond);
}

// static
constexpr TimeDelta TimeDelta::Max() {
  return TimeDelta(std::numeric_limits<int64_t>::max());
}

// static
constexpr TimeDelta TimeDelta::Min() {
  return TimeDelta(std::numeric_limits<int64_t>::min());
}

constexpr TimeDelta TimeDelta::operator+(TimeDelta other) const {
  if (!other.is_inf()) return TimeDelta(int64_t{delta_ + other.delta_});

  // Additions involving two infinities are only valid if signs match.
  CHECK(!is_inf() || (delta_ == other.delta_));
  return other;
}

constexpr TimeDelta TimeDelta::operator-(TimeDelta other) const {
  if (!other.is_inf()) return TimeDelta(int64_t{delta_ - other.delta_});

  // Subtractions involving two infinities are only valid if signs differ.
  CHECK_NE(delta_, other.delta_);
  return (other.delta_ < 0) ? Max() : Min();
}

inline constexpr double TimeDelta::InSecondsF() const {
  if (!is_inf()) return static_cast<double>(delta_) / kMicrosecondsPerSecond;
  return (delta_ < 0) ? -std::numeric_limits<double>::infinity()
                      : std::numeric_limits<double>::infinity();
}

}  // namespace media

#endif  // MEDIA_BASE_TIME_DELTA_H_
