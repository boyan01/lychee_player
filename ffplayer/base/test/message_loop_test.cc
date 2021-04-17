//
// Created by yangbin on 2021/3/28.
//
#include <thread>

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "base/logging.h"
#include "base/message_loop.h"
#include "base/bind_to_current_loop.h"

using media::base::MessageLoop;

typedef testing::MockFunction<void(void)> MockTask;

class MessageLoopTest : public testing::Test {

 public:
  MessageLoopTest() {
    message_loop_ = MessageLoop::prepare_looper("test_looper");
  }

  ~MessageLoopTest() override {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    message_loop_->Quit();
  }

 protected:
  MessageLoop *message_loop_ = nullptr;
};

static void quit_message_loop_with_wait(MessageLoop *message_loop) {
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  message_loop->Quit();
}

static MessageLoop *create_test_looper() {
  return MessageLoop::prepare_looper("test_loop");
}

#define TEST_MESSAGE_LOOP_START(test_name) \
TEST(MessageLoop, test_name) {             \
auto *message_loop = create_test_looper(); \


#define TEST_MESSAGE_LOOP_END()            \
quit_message_loop_with_wait(message_loop); \
}

TEST_MESSAGE_LOOP_START(TestOneLoop)
  MockTask task;
  EXPECT_CALL(task, Call()).Times(1);
  message_loop->PostTask(FROM_HERE, task.AsStdFunction());
TEST_MESSAGE_LOOP_END()

TEST_MESSAGE_LOOP_START(RunnerMessageLoop)
  MockTask task;
  EXPECT_CALL(task, Call()).Times(1).WillOnce([&]() {
    EXPECT_EQ(MessageLoop::current(), message_loop);
  });
  message_loop->PostTask(FROM_HERE, task.AsStdFunction());
TEST_MESSAGE_LOOP_END()

TEST_MESSAGE_LOOP_START(PostNestedTask)
  MockTask task;
  EXPECT_CALL(task, Call()).Times(1);
  message_loop->PostTask(FROM_HERE, [&]() {
    message_loop->PostTask(FROM_HERE_WITH_EXPLICIT_FUNCTION("PostNestedTask#lambda"), task.AsStdFunction());
  });
TEST_MESSAGE_LOOP_END()

TEST_MESSAGE_LOOP_START(CurrentThread)
  message_loop->PostTask(FROM_HERE, [&]() {
    EXPECT_TRUE(message_loop->BelongsToCurrentThread());
  });
TEST_MESSAGE_LOOP_END()

TEST_F(MessageLoopTest, BindToCurrent) {
  testing::MockFunction<void(int)> function1;
  testing::MockFunction<void(int)> function2;

  testing::InSequence in_sequence;
  EXPECT_CALL(function2, Call(0));
  EXPECT_CALL(function1, Call(0));

  message_loop_->PostTask(FROM_HERE, [&] {

    auto function1_bound = media::BindToCurrentLoop(function1.AsStdFunction());
    function1_bound(0);
    function2.AsStdFunction()(0);

  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

}

