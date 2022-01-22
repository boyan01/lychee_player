//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_H_
#define MEDIA_BASE_MESSAGE_H_

#include <functional>
#include <queue>

#include "base/location.h"
#include "base/time_delta.h"
#include "base/time_ticks.h"

namespace media {

class TaskRunner;

namespace base {

typedef std::function<void(void)> TaskClosure;

struct Message {
  Message(TaskClosure task, const tracked_objects::Location &posted_from,
          TimeDelta delay, TaskRunner *task_runner, int task_id);

  ~Message();

  // Used to support sorting.
  bool operator<(const Message &other) const;

  // The task to run
  TaskClosure task;

  tracked_objects::Location posted_from;

  int sequence_num;

  TimeTicks when;

 private:
  Message *next = nullptr;
  TaskRunner *task_runner_;
  int task_id_;

  friend class MessageQueue;
  friend class MessageLooper;
};

}  // namespace base
}  // namespace media

#endif  // MEDIA_BASE_MESSAGE_H_
