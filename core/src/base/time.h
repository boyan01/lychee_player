#ifndef LYCHEE_BASE_TIME_H_
#define LYCHEE_BASE_TIME_H_

#include <cstdint>
#include <limits>

namespace lychee {

constexpr int64_t kNoTimestamp = std::numeric_limits<int64_t>::min();
constexpr int64_t kInfiniteTime = std::numeric_limits<int64_t>::max();
constexpr int64_t kMicrosPerSecond = 1000000;
constexpr int64_t kMicrosPerMilli = 1000;

// Monotonic clock in microseconds: the timebase of media timing and the
// public lychee_now_us(). Under virtual time, the time last set with
// SetVirtualTime().
int64_t NowMicros();

// Real monotonic time in microseconds, for measuring how long work takes.
int64_t MonotonicMicros();

// Virtual time, for tests: time stands still until the test moves it, so
// playback runs as fast as decoding allows and does not depend on load.
// Enable it before creating any player; it cannot be turned off. Starts at
// the current real time.
void EnableVirtualTime();
bool IsVirtualTime();
// Moves virtual time forward to |now|; earlier times are ignored.
void SetVirtualTime(int64_t now);

#if defined(__APPLE__)
// Converts a mach_absolute_time() value to the NowMicros() timebase.
int64_t HostTimeToMicros(uint64_t host_time);
#endif

}  // namespace lychee

#endif  // LYCHEE_BASE_TIME_H_
