#include <dlfcn.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

#include "lychee/lychee.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// Simulate an unresponsive codec at its input boundary, then let the test
// finish even if the decoder fails to abort. All other calls use MediaCodec.
Clock::time_point stall_until;
std::atomic<bool> input_stalled{false};

bool CheckCancellation(const char* url, bool destroy) {
  lychee_thumbnailer* thumbnailer =
      lychee_thumbnailer_create(url, nullptr, 320, 180);
  if (!thumbnailer) {
    return false;
  }
  input_stalled = false;
  stall_until = Clock::now() + 3s;
  auto request = std::async(std::launch::async, [thumbnailer] {
    return lychee_thumbnailer_get(thumbnailer, 500000, 1);
  });
  while (!input_stalled && Clock::now() < stall_until &&
         request.wait_for(2ms) != std::future_status::ready) {
  }
  const bool stalled = input_stalled.load();
  const auto start = Clock::now();
  if (destroy && stalled) {
    lychee_thumbnailer_destroy(thumbnailer);
  } else {
    lychee_thumbnailer_cancel(thumbnailer);
  }
  lychee_frame* frame = request.get();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - start)
                           .count();
  const bool cancelled = stalled && !frame && elapsed < 500;
  lychee_frame_release(frame);
  std::printf("%s: input_stalled=%d returned_null=%d wait=%lld ms\n",
              destroy ? "destroy" : "cancel", stalled, frame == nullptr,
              static_cast<long long>(elapsed));
  if (destroy && stalled) {
    return cancelled;
  }

  stall_until = {};
  frame = lychee_thumbnailer_get(thumbnailer, 500000, 1);
  const bool reused = frame &&
                      lychee_frame_get_type(frame) == LYCHEE_FRAME_CPU_RGBA &&
                      lychee_frame_get_pts(frame) == 500000;
  std::printf("reuse: recovered=%d pts=%lld\n", reused,
              frame ? static_cast<long long>(lychee_frame_get_pts(frame)) : -1);
  lychee_frame_release(frame);
  lychee_thumbnailer_destroy(thumbnailer);
  return cancelled && reused;
}

}  // namespace

extern "C" ssize_t __real_AMediaCodec_dequeueInputBuffer(AMediaCodec* codec,
                                                         int64_t timeout_us);

extern "C" ssize_t __wrap_AMediaCodec_dequeueInputBuffer(AMediaCodec* codec,
                                                         int64_t timeout_us) {
  if (Clock::now() < stall_until) {
    input_stalled = true;
    std::this_thread::sleep_for(std::chrono::microseconds(timeout_us));
    return AMEDIACODEC_INFO_TRY_AGAIN_LATER;
  }
  if (input_stalled && stall_until != Clock::time_point{}) {
    return AMEDIA_ERROR_UNKNOWN;
  }
  return __real_AMediaCodec_dequeueInputBuffer(codec, timeout_us);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s h264_aac.mp4\n", argv[0]);
    return 1;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // An app has Binder workers already. The shell process needs them for
  // the codec's buffer-pool callbacks. The NDK entry point starts at API 29.
  void* binder = dlopen("libbinder_ndk.so", RTLD_NOW);
  if (binder) {
    auto start = reinterpret_cast<void (*)()>(
        dlsym(binder, "ABinderProcess_startThreadPool"));
    if (start) {
      start();
    }
  }
  const bool cancelled = CheckCancellation(argv[1], false);
  const bool destroyed = CheckCancellation(argv[1], true);
  return cancelled && destroyed ? 0 : 1;
}
