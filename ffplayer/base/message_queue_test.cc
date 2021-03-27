//
// Created by boyan on 2021/3/27.
//

#include "gtest/gtest.h"

#include "message_queue.h"

TEST(MessageQueue, PUSH) {
  EXPECT_EQ(1 + 1, 2);
  auto c = FROM_HERE;
  std::cout << c.ToString() << std::endl;
}