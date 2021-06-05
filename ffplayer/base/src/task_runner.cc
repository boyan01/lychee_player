//
// Created by Bin Yang on 2021/6/5.
//

#include <base/task_runner.h>
#include <base/logging.h>

namespace media {

TaskRunner::TaskRunner(MessageLoop *looper) : looper_(looper) {
  if (looper_ == nullptr) {
    looper_ = MessageLoop::current();
  }
  DCHECK(looper_) << "can not obtain looper from current thread.";
}

TaskRunner::~TaskRunner() = default;

void TaskRunner::PostTask(const tracked_objects::Location &from_here, const TaskClosure &task) {
  looper_->PostTask(from_here, task);
}

void TaskRunner::PostDelayedTask(const tracked_objects::Location &from_here,
                                 TimeDelta delay,
                                 const TaskClosure &task_closure) {
  looper_->PostDelayedTask(from_here, delay, task_closure);
}

}
