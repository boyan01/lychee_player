//
// Created by boyan on 2021/3/27.
//
#include <thread>

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "logging.h"

#include "message_queue.h"

using namespace media::base;
using namespace std::chrono;

static const Message &obtain_emtpy_message() {
  const static auto empty_lambda = []() {};
  const static Message message(empty_lambda, FROM_HERE);
  return message;
}

static void simple_loop(
    const std::function<void(MessageQueue &queue)> &before_prepared,
    const std::function<void(MessageQueue &queue)> &on_prepared,
    std::function<void(MessageQueue &queue, Message *)> on_message,
    milliseconds max_waiting_duration = milliseconds(3000) // 3 seconds.
) {

  MessageQueue queue;

  before_prepared(queue);

  std::thread looper_thread([&]() {
    while (true) {
      auto *next = queue.next();
      if (next == nullptr) {
        break;
      }
      on_message(queue, next);
    }
    DLOG(INFO) << "Simple Loop Quit.";
  });

  on_prepared(queue);

  std::this_thread::sleep_for(max_waiting_duration);
  queue.Quit();
  looper_thread.join();
}

static void simple_loop(
    const std::function<void(MessageQueue &queue)> &on_prepared,
    const std::function<void(MessageQueue &queue, Message *)> &on_message,
    milliseconds max_waiting_duration = milliseconds(3000) // 3 seconds.
) {
  simple_loop([](MessageQueue &queue) {}, on_prepared, on_message, max_waiting_duration);
}

TEST(MessageQueue, SimpleEnqueueMessage) {
  testing::MockFunction<bool(MessageQueue &queue, Message *)> on_message;
  EXPECT_CALL(on_message, Call(testing::_, testing::_))
      .Times(1);

  simple_loop(
      [](MessageQueue &queue) {
        queue.EnqueueMessage(obtain_emtpy_message());
      },
      on_message.AsStdFunction()
  );

}

TEST(MessageQueue, EnqueueMultiTimes) {

  const static int TEST_LOOP = 50;

  testing::MockFunction<bool(MessageQueue &queue, Message *)> on_message;
  EXPECT_CALL(on_message, Call(testing::_, testing::_))
      .Times(TEST_LOOP * 2);

  auto enqueue_messages = [](MessageQueue &queue) {
    for (int i = 0; i < TEST_LOOP; ++i) {
      queue.EnqueueMessage(obtain_emtpy_message());
      // sleep 10 millis for every simple loop
      std::this_thread::sleep_for(milliseconds(20));
    }
  };

  simple_loop(
      enqueue_messages,
      enqueue_messages,
      on_message.AsStdFunction()
  );
}