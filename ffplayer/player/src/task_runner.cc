//
// Created by yangbin on 2021/4/17.
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

void TaskRunner::PostTask(const tracked_objects::Location &from_here, const TaskClosure &task) {
  looper_->PostTask(from_here, task);
}

}
