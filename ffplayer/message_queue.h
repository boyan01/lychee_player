//
// Created by yangbin on 2022/8/7.
//

#ifndef MEDIA__MESSAGE_QUEUE_H_
#define MEDIA__MESSAGE_QUEUE_H_

#include <condition_variable>
#include <mutex>
#include <utility>
#include "basictypes.h"

struct Message {
  int what;
  int64_t arg1;
  int64_t arg2;
  struct Message* next;
};

class MessageQueue {
 public:
  MessageQueue();

  virtual ~MessageQueue();

  bool EnqueueMessage(const Message& message);

  Message* next();

  void Quit();

 private:
  Message* messages_ = nullptr;

  std::recursive_mutex message_queue_lock_;

  std::condition_variable_any message_wait_condition_;
  std::mutex message_wait_lock_;

  bool quitting_ = false;

  bool blocked_ = false;

  void Wake();

  void PollOnce();

  DELETE_COPY_AND_ASSIGN(MessageQueue);

  void RemoveAllMessageLocked();
};

#endif  // MEDIA__MESSAGE_QUEUE_H_
