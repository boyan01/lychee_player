//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_LOOP_H_
#define MEDIA_BASE_MESSAGE_LOOP_H_

#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "location.h"
#include "message_queue.h"

namespace media {
namespace base {

class MessageLooper {

 public:

  /**
   *
   *  Create MessageLoop and run in new thread.
   *
   * @param loop_name the name of [MessageLoop]
   * @return created MessageLoop.
   */
  static MessageLooper *PrepareLooper(const char *loop_name);

  explicit MessageLooper(const char *loop_name);

  void Prepare();

  virtual ~MessageLooper();

  // Returns the MessageLoop object for the current thread, or null if none.
  static MessageLooper *current();

  void PostTask(const tracked_objects::Location &from_here, const TaskClosure &task);

  void PostDelayedTask(const tracked_objects::Location &from_here,
                       TimeDelta delay,
                       const TaskClosure &task_closure);

  /**
   * @return true if the current thread is a thread on which is running current looper.
   */
  bool BelongsToCurrentThread() {
    return MessageLooper::current() == this;
  }

  void Loop();

  void Quit();

 private:

  friend class ::media::TaskRunner;

  bool prepared_ = false;

  const char *loop_name_;

  std::unique_ptr<MessageQueue> message_queue_;

  std::unique_ptr<std::thread> thread_;

  DELETE_COPY_AND_ASSIGN(MessageLooper);

  static thread_local MessageLooper *thread_local_message_loop_;

};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_LOOP_H_
