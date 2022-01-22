//
// Created by boyan on 2021/3/27.
//

#include "base/message_loop.h"

#include "base/logging.h"
#include "base/utility.h"

namespace media {

namespace base {

thread_local std::weak_ptr<MessageLooper>
    MessageLooper::thread_local_message_loop_;

MessageLooper::MessageLooper(const char *loop_name,
                             int message_handle_timeout_ms)
    : loop_name_(loop_name),
      message_queue_(std::make_unique<MessageQueue>()),
      message_handle_expect_duration_(message_handle_timeout_ms) {}

MessageLooper::~MessageLooper() {
  message_queue_->Quit();
  thread_->join();
}

void MessageLooper::Prepare() {
  DCHECK(!prepared_);
  prepared_ = true;
  utility::update_thread_name(loop_name_);
  thread_local_message_loop_ = shared_from_this();
}

void MessageLooper::Loop() {
  for (;;) {
    Message *msg = message_queue_->next();
    if (msg == nullptr) {
      DLOG(INFO) << "MessageLoop " << loop_name_ << " over.";
      return;
    }

    DCHECK(msg->next == nullptr);

    TRACE_METHOD_DURATION_WITH_LOCATION(message_handle_expect_duration_,
                                        msg->posted_from);
    msg->task();
    delete msg;
  }
}

void MessageLooper::PostTask(const tracked_objects::Location &from_here,
                             const TaskClosure &task) {
  static const TimeDelta delay;
  PostDelayedTask(from_here, delay, task);
}
void MessageLooper::PostDelayedTask(const tracked_objects::Location &from_here,
                                    TimeDelta delay,
                                    const TaskClosure &task_closure) {
  Message message(task_closure, from_here, delay, nullptr, 0);
  message_queue_->EnqueueMessage(message);
}

std::shared_ptr<MessageLooper> MessageLooper::Current() {
  return thread_local_message_loop_.lock();
}

// static
std::shared_ptr<MessageLooper> MessageLooper::PrepareLooper(
    const char *loop_name, int message_handle_expect_duration_) {
  auto looper = Create(loop_name, message_handle_expect_duration_);
  auto thread = std::make_unique<std::thread>([looper]() {
    looper->Prepare();
    looper->Loop();
  });
  looper->thread_ = std::move(thread);
  return looper;
}

std::shared_ptr<MessageLooper> MessageLooper::Create(
    const char *loop_name, int message_handle_expect_duration_) {
  std::shared_ptr<MessageLooper> looper(
      new MessageLooper(loop_name, message_handle_expect_duration_));
  return looper;
}

}  // namespace base
}  // namespace media