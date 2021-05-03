//
// Created by yangbin on 2021/4/11.
//

#ifndef MEDIA_BASE_CIRCULAR_DEQUE_H_
#define MEDIA_BASE_CIRCULAR_DEQUE_H_

#include "vector"

#include "base/logging.h"

namespace media {

template<typename T>
class CircularDeque {

 public:
  explicit CircularDeque(int capacity) : capacity_(capacity),
                                         deque_(capacity),
                                         front_(0),
                                         behind_(0),
                                         empty_(true),
                                         full_(false) {
    DCHECK_GT(capacity_, 0);
  }

  bool InsertFront(T value) {
    if (full_) return false;
    front_ = (front_ - 1 + capacity_) % capacity_;
    deque_[front_] = value;
    empty_ = false;
    if (front_ == behind_) full_ = true;
    return true;
  }

  bool InsertLast(T value) {
    if (full_) return false;
    deque_[behind_] = value;
    behind_ = (behind_ + 1) % capacity_;
    empty_ = false;
    if (behind_ == front_) full_ = true;
    return true;
  }

  bool DeleteFront() {
    if (empty_) return false;
    front_ = (front_ + 1) % capacity_;
    full_ = false;
    if (front_ == behind_) empty_ = true;
    return true;
  }

  T PopFront() {
    DCHECK(!empty_);
    auto item = deque_[front_];
    DeleteFront();
    return item;
  }

  bool DeleteLast() {
    if (empty_) return false;
    behind_ = (behind_ - 1 + capacity_) % capacity_;
    full_ = false;
    if (front_ == behind_) empty_ = true;
    return true;
  }

  /**
   * @return The front item from the deque.
   */
  const T &GetFront() const {
    return deque_[front_];
  }

  /**
   * @return The front item from the deque.
   */
  T &GetFront() {
    return deque_[front_];
  }

  /**
   * Get the last item from the deque.
   */
  const T &GetRear() const {
    return deque_[(behind_ - 1 + capacity_) % capacity_];
  }

  T &GetRear() {
    return deque_[(behind_ - 1 + capacity_) % capacity_];
  }

  bool IsEmpty() const { return empty_; }

  bool IsFull() const { return full_; }

  int GetSize() const {
    if (empty_) return 0;
    if (full_) return capacity_;
    return abs(behind_ - front_);
  }

  void Clear() {
    if (empty_) return;
    full_ = false;
    empty_ = true;
    front_ = behind_ = 0;
  }

 private:
  int capacity_;
  std::vector<T> deque_;
  int front_;
  int behind_;
  bool empty_;
  bool full_;

};

}

#endif //MEDIA_BASE_CIRCULAR_DEQUE_H_
