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

  /**
   *
   *  Create MessageLoop and run in new thread.
   *
   * @param loop_name the name of [MessageLoop]
   * @return created MessageLoop.
   */
  static MessageLoop *prepare_looper(const char *loop_name);

  virtual ~MessageLoop();

  // Returns the MessageLoop object for the current thread, or null if none.
  static MessageLoop *current();

  void PostTask(const tracked_objects::Location &from_here, const TaskClosure &task);

  /**
   * @return true if the current thread is a thread on which is running current looper.
   */
  bool BelongsToCurrentThread() {
    return MessageLoop::current() == this;
  }

  void Quit();

 private:

  explicit MessageLoop(const char *loop_name);

  const char *loop_name_;

  MessageQueue message_queue_;

  void Loop();

  DISALLOW_COPY_AND_ASSIGN(MessageLoop);

  static thread_local MessageLoop *thread_local_message_loop_;

};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_LOOP_H_
