//
// Created by yangbin on 2021/4/11.
//

#include "gtest/gtest.h"

#include "base/circular_deque.h"

struct CircularDequeTestValue {
  int value;
  double state;
};

TEST(CircularDequeTest, Insert) {
  media::CircularDeque<CircularDequeTestValue> deque(10);

  EXPECT_TRUE(deque.IsEmpty());
  EXPECT_FALSE(deque.IsFull());

  CircularDequeTestValue value{10, 20.0};
  deque.InsertFront(value);

  EXPECT_FALSE(deque.IsEmpty());
  EXPECT_FALSE(deque.IsFull());

  deque.DeleteLast();

  EXPECT_TRUE(deque.IsEmpty());
  EXPECT_FALSE(deque.IsFull());

}
