#include "base/time.h"

#include <atomic>

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

int64_t MonotonicMicros() {
  return HostTimeToMicros(mach_absolute_time());
}

#else

int64_t MonotonicMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#endif

namespace {

std::atomic<bool> g_virtual{false};
std::atomic<int64_t> g_virtual_now{0};

}  // namespace

int64_t NowMicros() {
  if (g_virtual.load(std::memory_order_acquire)) {
    return g_virtual_now.load(std::memory_order_acquire);
  }
  return MonotonicMicros();
}

void EnableVirtualTime() {
  if (!g_virtual.load()) {
    g_virtual_now.store(MonotonicMicros());
    g_virtual.store(true, std::memory_order_release);
  }
}

bool IsVirtualTime() {
  return g_virtual.load(std::memory_order_acquire);
}

void SetVirtualTime(int64_t now) {
  int64_t current = g_virtual_now.load();
  while (now > current &&
         !g_virtual_now.compare_exchange_weak(current, now)) {
  }
}

}  // namespace lychee
