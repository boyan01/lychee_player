//
// Created by Bin Yang on 2021/6/13.
//

#include "demuxer.h"

#include "base/test_helper.h"
#include "condition_variable"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "utility"

using namespace media;

namespace {

class FakeDemuxerHost : public DemuxerHost {
 public:
  MOCK_METHOD(void, SetDuration, (double));
  MOCK_METHOD(void, OnDemuxerError, (PipelineStatus));
};

}  // namespace

class DemuxerTest : public testing::Test {
 protected:
  std::shared_ptr<MessageLooper> player_looper;
  std::shared_ptr<Demuxer> demuxer_;
  FakeDemuxerHost demuxer_host_;
  testing::MockFunction<void(int)> status_cb_;

  testing::MockFunction<void(std::unique_ptr<MediaTracks>)>
      media_tracks_update_callback_;

  void SetUp() override {
    player_looper = MessageLooper::PrepareLooper("task_main");
  }

  void CreateDemuxer(const std::string &name) {
    DCHECK(!demuxer_);
    auto task_runner = TaskRunner(MessageLooper::PrepareLooper("demuxer"));
    demuxer_ = std::make_shared<Demuxer>(
        task_runner, name, media_tracks_update_callback_.AsStdFunction());
  }

  void InitializeDemuxer() {
    DCHECK(demuxer_);
    auto count_down_latch = std::make_unique<media_test::CountDownLatch>(1);
    EXPECT_CALL(status_cb_, Call(testing::_))
        .Times(1)
        .WillOnce([&](int status) {
          EXPECT_EQ(status, 0);
          count_down_latch->CountDown();
        });
    player_looper->PostTask(FROM_HERE, [&]() {
      demuxer_->Initialize(&demuxer_host_, status_cb_.AsStdFunction());
    });
    EXPECT_TRUE(count_down_latch->Wait(TimeDelta::FromSeconds(4)));
  }
};

TEST_F(DemuxerTest, OpenFile) {
  CreateDemuxer("/Users/yangbin/Movies/good_words.mp4");
  InitializeDemuxer();
}