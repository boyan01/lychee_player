//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_QUEUE_H_
#define MEDIA_BASE_MESSAGE_QUEUE_H_

#include <mutex>
#include <condition_variable>

#include "basictypes.h"
#include "message.h"

namespace media {
namespace base {

class MessageQueue {

 public:

  MessageQueue();

  virtual ~MessageQueue();

  bool EnqueueMessage(const Message &message);

  Message *next();

  void Quit();
 private:

  Message *messages_ = nullptr;

  std::recursive_mutex message_queue_lock_;

  std::condition_variable_any message_wait_condition_;
  std::mutex message_wait_lock_;

  bool quitting_ = false;

  bool blocked_ = false;

  void Wake();

  void PollOnce(std::chrono::milliseconds wait_duration);

  DISALLOW_COPY_AND_ASSIGN(MessageQueue);

  void RemoveAllMessageLocked();
};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_QUEUE_H_
