//
// Created by yangbin on 2021/4/11.
//

#include "base/circular_deque.h"

#include "gtest/gtest.h"

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

  deque.InsertFront(value);
  deque.InsertFront(value);
  deque.InsertFront(value);
  EXPECT_EQ(deque.GetSize(), 3);

  deque.Clear();
  EXPECT_EQ(deque.GetSize(), 0);
  EXPECT_EQ(deque.IsEmpty(), true);
  EXPECT_EQ(deque.IsFull(), false);
}
