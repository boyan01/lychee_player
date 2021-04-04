//
// Created by boyan on 2021/3/27.
//

#include "base/logging.h"
#include "base/message_queue.h"

namespace media {
namespace base {

MessageQueue::MessageQueue() = default;

MessageQueue::~MessageQueue() {
  Quit();
}

bool MessageQueue::EnqueueMessage(const Message &message) {
  std::unique_lock<std::recursive_mutex> auto_lock(message_queue_lock_);

  if (quitting_) {
    NOTREACHED() << "sending message on a dead thread";
    return false;
  }
  auto *msg = new Message(message);

  Message *p = messages_;
  bool needWake;

  if (p == nullptr || msg->when < p->when) {
    msg->next = p;
    messages_ = msg;
    needWake = blocked_;
  } else {
    // Inserted withing the middle of the queue, We don't have to wake up the event queue.
    needWake = false;
    Message *prev;
    for (;;) {
      prev = p;
      p = p->next;
      if (p == nullptr || msg->when < p->when) {
        break;
      }
    }
    msg->next = p;
    prev->next = msg;
  }

  if (needWake) {
    Wake();
  }
  return true;
}

Message *MessageQueue::next() {
  if (quitting_) {
    return nullptr;
  }

  std::chrono::milliseconds next_poll_timeout_mills(0);
  for (;;) {
    PollOnce(next_poll_timeout_mills);
    {
      std::lock_guard<std::recursive_mutex> auto_lock(message_queue_lock_);

      auto now = std::chrono::system_clock::now();

      Message *msg = messages_;

      if (msg != nullptr) {
        if (now < msg->when) {
          next_poll_timeout_mills = std::chrono::duration_cast<std::chrono::milliseconds>(msg->when - now);
        } else {
          blocked_ = false;
          messages_ = msg->next;
          msg->next = nullptr;
          return msg;
        }
      } else {
        next_poll_timeout_mills = std::chrono::milliseconds(-1);
      }

      if (quitting_) {
        break;
      }

      blocked_ = true;
      continue;
    }

  }
  return nullptr;
}

void MessageQueue::Wake() {
  std::lock_guard<std::mutex> condition_lock(message_wait_lock_);
  message_wait_condition_.notify_one();
}

void MessageQueue::PollOnce(std::chrono::milliseconds wait_duration) {
  std::unique_lock<std::mutex> condition_lock(message_wait_lock_);
  if (wait_duration >= std::chrono::milliseconds::zero()) {
    message_wait_condition_.wait_for(condition_lock, wait_duration);
  } else {
    message_wait_condition_.wait(condition_lock);
  }
}

void MessageQueue::Quit() {
  std::lock_guard<std::recursive_mutex> auto_lock(message_queue_lock_);
  if (quitting_) {
    return;
  }
  quitting_ = true;
  RemoveAllMessageLocked();
  Wake();
}

void MessageQueue::RemoveAllMessageLocked() {
  Message *p = messages_;
  while (p != nullptr) {
    Message *n = p->next;
    delete p;
    p = n;
  }
  messages_ = nullptr;
}

} // namespace base
} // namespace media