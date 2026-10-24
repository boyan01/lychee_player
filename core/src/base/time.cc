#include "base/time.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <chrono>
#endif

namespace lychee {

#if defined(__APPLE__)

int64_t HostTimeToMicros(uint64_t host_time) {
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info;
    mach_timebase_info(&info);
    return info;
  }();
  // 128-bit math avoids overflowing on long uptimes.
  __uint128_t nanos = static_cast<__uint128_t>(host_time) * timebase.numer /
                      timebase.denom;
  return static_cast<int64_t>(nanos / 1000);
}

int64_t NowMicros() {
  return HostTimeToMicros(mach_absolute_time());
}

#else

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#endif

}  // namespace lychee
