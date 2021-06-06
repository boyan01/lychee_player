//
// Created by Bin Yang on 2021/6/5.
//

#include <base/task_runner.h>
#include <base/logging.h>

namespace media {

TaskRunner::TaskRunner(MessageLooper *looper) : looper_(looper), message_queue_(nullptr) {
  if (looper_ == nullptr) {
    looper_ = MessageLooper::current();
  }
  CHECK(looper_) << "can not obtain looper from current thread.";
  message_queue_ = looper_->message_queue_.get();
  CHECK(message_queue_) << "failed to get message queue.";
}

TaskRunner::~TaskRunner() {
  RemoveAllTasks();
}

void TaskRunner::PostTask(const tracked_objects::Location &from_here, const TaskClosure &task) {
  PostTask(from_here, 0, task);
}

void TaskRunner::PostTask(const tracked_objects::Location &from_here, int task_id, const TaskClosure &task) {
  static const TimeDelta delay(0);
  PostDelayedTask(from_here, delay, task_id, task);
}

void TaskRunner::PostDelayedTask(const tracked_objects::Location &from_here,
                                 TimeDelta delay,
                                 const TaskClosure &task_closure) {
  PostDelayedTask(from_here, delay, 0, task_closure);
}

void TaskRunner::PostDelayedTask(
    const tracked_objects::Location &from_here,
    TimeDelta delay,
    int task_id,
    const TaskClosure &task_closure) {
  Message message(task_closure, from_here, delay, this, task_id);
  message_queue_->EnqueueMessage(message);
}

void TaskRunner::RemoveTask(int task_id) {
  message_queue_->RemoveTask(this, task_id);
}

void TaskRunner::RemoveAllTasks() {
  message_queue_->RemoveTask(this);
}

bool TaskRunner::BelongsToCurrentThread() {
  return looper_->BelongsToCurrentThread();
}

}
