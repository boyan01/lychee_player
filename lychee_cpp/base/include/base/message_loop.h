//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_LOOP_H_
#define MEDIA_BASE_MESSAGE_LOOP_H_

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "location.h"
#include "message_queue.h"

namespace media {
namespace base {

class MessageLooper : public std::enable_shared_from_this<MessageLooper> {
 public:
  /**
   *
   *  Create MessageLoop and run in new thread.
   *
   * @param loop_name the name of [MessageLoop]
   * @return created MessageLoop.
   */
  static std::shared_ptr<MessageLooper> PrepareLooper(
      const char *loop_name,
      int message_handle_expect_duration_ = std::numeric_limits<int>::max());

  static std::shared_ptr<MessageLooper> Create(
      const char *loop_name,
      int message_handle_expect_duration_ = std::numeric_limits<int>::max());

  void Prepare();

  virtual ~MessageLooper();

  // Returns the MessageLoop object for the current thread, or null if none.
  static std::shared_ptr<MessageLooper> Current();

  void PostTask(const tracked_objects::Location &from_here,
                const TaskClosure &task);

  void PostDelayedTask(const tracked_objects::Location &from_here,
                       TimeDelta delay, const TaskClosure &task_closure);

  /**
   * @return true if the current thread is a thread on which is running current
   * looper.
   */
  bool BelongsToCurrentThread() {
    return MessageLooper::Current().get() == this;
  }

  void Loop();

 private:
  explicit MessageLooper(
      const char *loop_name,
      int message_handle_timeout_ms = std::numeric_limits<int>::max());

  friend class ::media::TaskRunner;

  bool prepared_ = false;

  const char *loop_name_;

  std::unique_ptr<MessageQueue> message_queue_;

  std::unique_ptr<std::thread> thread_;

  int message_handle_expect_duration_;

  Message *current_processing_message_;
  std::mutex current_processing_message_mutex_;

  DELETE_COPY_AND_ASSIGN(MessageLooper);

  static thread_local std::weak_ptr<MessageLooper> thread_local_message_loop_;
};

}  // namespace base
}  // namespace media

#endif  // MEDIA_BASE_MESSAGE_LOOP_H_
