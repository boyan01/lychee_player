//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_H_
#define MEDIA_BASE_MESSAGE_H_

#include <queue>
#include <functional>
#include <chrono>

#include "location.h"

namespace media {
namespace base {

typedef std::function<void(void)> TaskClosure;

struct Message {

  Message(TaskClosure task,
          const tracked_objects::Location &posted_from);

  Message(TaskClosure task,
          const tracked_objects::Location &posted_from,
          const std::chrono::milliseconds &delayed_run_time);

  ~Message();

  // Used to support sorting.
  bool operator<(const Message &other) const;

  // The task to run
  TaskClosure task;

  tracked_objects::Location posted_from;

  int sequence_num;

  std::chrono::time_point<std::chrono::system_clock> when;

  Message *next = nullptr;

};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_H_
