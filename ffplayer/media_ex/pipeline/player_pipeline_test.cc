//
// Created by yangbin on 2021/4/6.
//

#include "thread"

#include "pipeline/player_pipeline.h"

#include "gtest/gtest.h"
#include "gmock/gmock.h"

namespace media {

using testing::MockFunction;
using testing::Return;

const static auto test_file = "/Users/yangbin/Pictures/mojito.mp4";

class PlayerPipelineTest : public testing::Test {
 public:
  PlayerPipelineTest() : player_pipeline_(new PlayerPipeline()) {}

  ~PlayerPipelineTest() override {
    delete player_pipeline_;
  }

 protected:
  PlayerPipeline *player_pipeline_;

};

TEST_F(PlayerPipelineTest, OpenSource) {
  MockFunction<void(bool)> open_cb;
  EXPECT_CALL(open_cb, Call(true)).WillOnce(Return());

  player_pipeline_->Open(test_file, open_cb.AsStdFunction());
  std::this_thread::sleep_for(chrono::milliseconds(200));

}

}
