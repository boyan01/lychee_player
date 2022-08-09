//
// Created by yangbin on 2022/8/7.
//

#include "message_queue.h"

#include "logging.h"

MessageQueue::MessageQueue() = default;

MessageQueue::~MessageQueue() {
  DCHECK(quitting_);
}

bool MessageQueue::EnqueueMessage(const Message& message) {
  std::unique_lock<std::recursive_mutex> auto_lock(message_queue_lock_);

  if (quitting_) {
    DLOG(WARNING) << "sending message on a dead thread";
    return false;
  }
  auto* msg = new Message(message);

  Message* p = messages_;
  bool needWake;

  if (p == nullptr) {
    msg->next = p;
    messages_ = msg;
    needWake = blocked_;
  } else {
    // Inserted withing the middle of the queue, We don't have to wake up the
    // event queue.
    needWake = false;
    Message* prev;
    for (;;) {
      prev = p;
      p = p->next;
      if (p == nullptr) {
        break;
      }
      if (msg->what == p->what && msg->arg1 == p->arg1 &&
          msg->arg2 == p->arg2) {
        // already have this message, delete the new one and return.
        delete msg;
        return true;
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

Message* MessageQueue::next() {
  if (quitting_) {
    return nullptr;
  }

  for (;;) {
    {
      std::lock_guard<std::recursive_mutex> auto_lock(message_queue_lock_);

      Message* msg = messages_;

      if (msg != nullptr) {
        blocked_ = false;
        messages_ = msg->next;
        msg->next = nullptr;
        return msg;
      }

      if (quitting_) {
        break;
      }

      blocked_ = true;
    }
    PollOnce();
  }
  return nullptr;
}

void MessageQueue::Wake() {
  std::lock_guard<std::mutex> condition_lock(message_wait_lock_);
  message_wait_condition_.notify_one();
}

void MessageQueue::PollOnce() {
  std::unique_lock<std::mutex> condition_lock(message_wait_lock_);
  message_wait_condition_.wait_for(condition_lock,
                                   std::chrono::milliseconds(300));
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
  Message* p = messages_;
  while (p != nullptr) {
    Message* n = p->next;
    delete p;
    p = n;
  }
  messages_ = nullptr;
}
