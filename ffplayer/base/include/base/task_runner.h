//
// Created by Bin Yang on 2021/6/5.
//

#ifndef MEDIA_BASE_TASK_RUNNER_H_
#define MEDIA_BASE_TASK_RUNNER_H_

#include "message_loop.h"

namespace media {

using namespace media::base;

/**
 * TaskRunner to schedule and manger tasks.
 */
class TaskRunner {

 public:

  explicit TaskRunner(MessageLoop *looper = nullptr);

  void PostTask(const tracked_objects::Location &from_here, const TaskClosure &task);

  void PostDelayedTask(const tracked_objects::Location &from_here,
                       TimeDelta delay,
                       const TaskClosure &task_closure);

 private:

  MessageLoop *looper_;

};

}
#endif //MEDIA_BASE_TASK_RUNNER_H_
