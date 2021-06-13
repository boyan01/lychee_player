//
// Created by Bin Yang on 2021/6/13.
//

#include "demuxer.h"

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "condition_variable"

using namespace media;

namespace {

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

  bool Wait(TimeDelta time_delta) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto status = condition_.wait_for(lock, chrono::microseconds(time_delta.InMicroseconds()));
    return status == std::cv_status::no_timeout;
  }

 private:
  std::mutex mutex_;
  std::condition_variable_any condition_;

  int num_;

};

class FakeDemuxerHost : public DemuxerHost {
 public:
  MOCK_METHOD(void, SetDuration, (double));
  MOCK_METHOD(void, OnDemuxerError, (PipelineStatus));
};

}

class DemuxerTest : public testing::Test {

 protected:

  void SetUp() override {
    player_looper = MessageLooper::PrepareLooper("task_main");
  }

  std::shared_ptr<MessageLooper> player_looper;
};

TEST_F(DemuxerTest, OpenFile) {
  testing::MockFunction<void(std::unique_ptr<MediaTracks>)> media_tracks_update_callback;
  auto task_runner = TaskRunner(MessageLooper::PrepareLooper("demuxer"));
  std::shared_ptr<Demuxer> demuxer = std::make_shared<Demuxer>(
      task_runner,
      "/Users/yangbin/Movies/good_words.mp4",
      media_tracks_update_callback.AsStdFunction()
  );

  auto count_down_latch = std::make_unique<CountDownLatch>(1);

  testing::MockFunction<void(int)> status_cb;

  EXPECT_CALL(status_cb, Call(testing::_)).Times(1).WillOnce([&](int status) {
    EXPECT_EQ(status, 0);
    count_down_latch->CountDown();
  });

  FakeDemuxerHost demuxer_host;

  player_looper->PostTask(FROM_HERE, [&]() {
    demuxer->Initialize(&demuxer_host, status_cb.AsStdFunction());
  });

  EXPECT_TRUE(count_down_latch->Wait(TimeDelta::FromSeconds(4)));
}