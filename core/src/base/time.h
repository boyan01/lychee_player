#ifndef LYCHEE_BASE_TIME_H_
#define LYCHEE_BASE_TIME_H_

#include <cstdint>
#include <limits>

namespace lychee {

constexpr int64_t kNoTimestamp = std::numeric_limits<int64_t>::min();
constexpr int64_t kInfiniteTime = std::numeric_limits<int64_t>::max();
constexpr int64_t kMicrosPerSecond = 1000000;
constexpr int64_t kMicrosPerMilli = 1000;

// Monotonic clock in microseconds. The public lychee_now_us().
int64_t NowMicros();

#if defined(__APPLE__)
// Converts a mach_absolute_time() value to the NowMicros() timebase.
int64_t HostTimeToMicros(uint64_t host_time);
#endif

}  // namespace lychee

#endif  // LYCHEE_BASE_TIME_H_
