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

  explicit TaskRunner(MessageLooper *looper = nullptr);

  virtual ~TaskRunner();

  void PostTask(const tracked_objects::Location &from_here, const TaskClosure &task);

  void PostTask(const tracked_objects::Location &from_here, int task_id, const TaskClosure &task);

  void PostDelayedTask(const tracked_objects::Location &from_here,
                       TimeDelta delay,
                       const TaskClosure &task_closure);

  void PostDelayedTask(const tracked_objects::Location &from_here,
                       TimeDelta delay,
                       int task_id,
                       const TaskClosure &task_closure);

  void RemoveTask(int task_id);

  void RemoveAllTasks();

  bool BelongsToCurrentThread();

 private:

  MessageLooper *looper_;

  MessageQueue *message_queue_;

};

}
#endif //MEDIA_BASE_TASK_RUNNER_H_
