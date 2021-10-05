//
// Created by Bin Yang on 2021/6/5.
//

#include <base/task_runner.h>
#include <base/logging.h>

namespace media {

TaskRunner TaskRunner::CreateFromCurrent() {
  auto looper = MessageLooper::Current();
  DCHECK(looper) << "failed to obtain looper in current thread.";
  return TaskRunner(looper);
}

TaskRunner::TaskRunner(std::shared_ptr<MessageLooper> looper) : looper_(std::move(looper)) {
}

TaskRunner::TaskRunner() = default;

TaskRunner::TaskRunner(const TaskRunner &object) = default;

TaskRunner::~TaskRunner() {
  if (!looper_) {
    return;
  }
  RemoveAllTasks();
}

void TaskRunner::PostTask(const tracked_objects::Location &from_here, const TaskClosure &task) {
  PostTask(from_here, 0, task);
}

void TaskRunner::PostTask(const tracked_objects::Location &from_here, int task_id, const TaskClosure &task) {
  static const TimeDelta delay;
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
  if (!looper_) {
    return;
  }
  Message message(task_closure, from_here, delay, this, task_id);
  looper_->message_queue_->EnqueueMessage(message);
}

void TaskRunner::RemoveTask(int task_id) {
  if (!looper_) {
    return;
  }
  looper_->message_queue_->RemoveTask(this, task_id);
}

void TaskRunner::RemoveAllTasks() {
  if (!looper_) {
    return;
  }
  looper_->message_queue_->RemoveTask(this);
}

bool TaskRunner::BelongsToCurrentThread() {
  DCHECK(looper_);
  return looper_->BelongsToCurrentThread();
}

void TaskRunner::Reset() {
  RemoveAllTasks();
  looper_ = nullptr;
}

TaskRunner &TaskRunner::operator=(std::nullptr_t) {
  Reset();
  return *this;
}

}
