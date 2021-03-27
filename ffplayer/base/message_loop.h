//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_LOOP_H_
#define MEDIA_BASE_MESSAGE_LOOP_H_

#include <functional>
#include <mutex>
#include <condition_variable>

#include "location.h"
#include "message_queue.h"

namespace media {
namespace base {


class MessageLoop {

 public:

  void PostTask(const tracked_objects::Location &from_here, TaskClosure &task);

 private:
  MessageQueue incoming_queue_;
  mutable std::mutex incoming_queue_lock_;

  // The next sequence number to use for delayed tasks. Updating this counter is
  // protected by incoming_queue_lock_.
  int next_sequence_num_;

  MessageQueue work_queue_;

  void AddToIncomingQueue(Message *message);

  void DoWork();

  void ReloadWorkQueue();

  void AddToDelayedWorkQueue(const Message &message);

  static void RunTask(const Message &message);
};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_LOOP_H_
