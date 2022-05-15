//
// Created by yangbin on 2021/6/6.
//

#include "base/task_runner.h"

#include <thread>

#include "base/bind_to_current_loop.h"
#include "base/logging.h"
#include "base/message_loop.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using media::TaskRunner;
using media::base::MessageLooper;

typedef testing::MockFunction<void(void)> MockTask;

class TaskRunnerTest : public testing::Test {
 protected:
  std::shared_ptr<MessageLooper> looper_;
  TaskRunner task_runner_;

  void TearDown() override {
    task_runner_ = nullptr;
    looper_ = nullptr;
  }

  void SetUp() override {
    looper_ = MessageLooper::PrepareLooper("test_looper");
    task_runner_ = TaskRunner(looper_);
  }
};

static void WaitSeconds() {
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
}

TEST_F(TaskRunnerTest, PostTask) {
  MockTask task;
  EXPECT_CALL(task, Call()).Times(1);
  task_runner_.PostTask(FROM_HERE, task.AsStdFunction());
  WaitSeconds();
}

TEST_F(TaskRunnerTest, PostTaskDelay) {
  MockTask task;
  EXPECT_CALL(task, Call()).Times(1);
  task_runner_.PostDelayedTask(FROM_HERE,
                               media::TimeDelta::FromMicroseconds(1000),
                               task.AsStdFunction());
  WaitSeconds();
}

TEST_F(TaskRunnerTest, PostTaskRuningThread) {
  MockTask task;
  EXPECT_CALL(task, Call()).Times(1).WillOnce([&]() {
    EXPECT_EQ(MessageLooper::Current(), looper_);
  });
  task_runner_.PostTask(FROM_HERE, task.AsStdFunction());
  WaitSeconds();
}

TEST_F(TaskRunnerTest, BelongToCurrentThread) {
  EXPECT_FALSE(looper_->BelongsToCurrentThread());
  EXPECT_FALSE(task_runner_.BelongsToCurrentThread());

  task_runner_.PostTask(FROM_HERE, [&]() {
    EXPECT_TRUE(looper_->BelongsToCurrentThread());
    EXPECT_TRUE(task_runner_.BelongsToCurrentThread());
  });

  WaitSeconds();
}

TEST_F(TaskRunnerTest, RemoveTask) {
  MockTask task;
  EXPECT_CALL(task, Call()).Times(0);
  task_runner_.PostDelayedTask(
      FROM_HERE, media::TimeDelta::FromMicroseconds(100), task.AsStdFunction());
  task_runner_.PostDelayedTask(
      FROM_HERE, media::TimeDelta::FromMicroseconds(100), task.AsStdFunction());
  task_runner_.RemoveAllTasks();
  WaitSeconds();
}

TEST_F(TaskRunnerTest, RemoveTaskWithTaskId) {
  static const int kTaskId = 10;

  MockTask task;
  EXPECT_CALL(task, Call()).Times(1);
  task_runner_.PostDelayedTask(
      FROM_HERE, media::TimeDelta::FromMicroseconds(100), task.AsStdFunction());
  task_runner_.PostDelayedTask(FROM_HERE,
                               media::TimeDelta::FromMicroseconds(100), kTaskId,
                               task.AsStdFunction());
  task_runner_.RemoveTask(kTaskId);
  WaitSeconds();
}
