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

  class Task {
   public:
    explicit Task(TaskClosure task_closure);
   private:
    TaskClosure task_closure_;
    DELETE_COPY_AND_ASSIGN(Task);

   public:
    friend TaskRunner;
  };

  explicit TaskRunner(MessageLoop *looper = nullptr);

  virtual ~TaskRunner();

  void PostTask(const tracked_objects::Location &from_here, const TaskClosure &task);

  void PostTask(const tracked_objects::Location &from_here, const std::shared_ptr<Task> &task) {

  }

  void PostDelayedTask(const tracked_objects::Location &from_here,
                       TimeDelta delay,
                       const TaskClosure &task_closure);

  bool RemoveTask(int task_id);

  bool RemoveAllTasks();

 private:

  MessageLoop *looper_;

};

}
#endif //MEDIA_BASE_TASK_RUNNER_H_
