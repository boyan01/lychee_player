//
// Created by Bin Yang on 2021/6/14.
//

#ifndef MEDIA_BASE_TEST_HELPER_H_
#define MEDIA_BASE_TEST_HELPER_H_

#include "base/time_delta.h"

namespace media_test {

class CountDownLatch {
 public:

  explicit CountDownLatch(int num) : num_(num) {}

  void CountDown() {
    std::lock_guard<std::mutex> lock(mutex_);
    num_--;
    if (num_ <= 0) {
      condition_.notify_one();
    }
  }

  bool Wait(media::TimeDelta time_delta) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto status = condition_.wait_for(lock, chrono::microseconds(time_delta.InMicroseconds()));
    return status == std::cv_status::no_timeout;
  }

 private:
  std::mutex mutex_;
  std::condition_variable_any condition_;

  int num_;

};

}

#endif //MEDIA_BASE_TEST_HELPER_H_
