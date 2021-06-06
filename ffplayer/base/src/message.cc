//
// Created by boyan on 2021/3/27.
//

#include "base/message.h"

#include <utility>

namespace media {
namespace base {

Message::Message(TaskClosure task,
                 const media::tracked_objects::Location &posted_from,
                 TimeDelta delayed_run_time,
                 TaskRunner *task_runner,
                 int task_id)
    : task(std::move(task)),
      posted_from(posted_from),
      when(std::chrono::system_clock::now() + delayed_run_time),
      sequence_num(0),
      task_runner_(task_runner),
      task_id_(task_id) {

}

bool Message::operator<(const Message &other) const {
  if (when > other.when)
    return true;

  // If the times happen to match, then we use the sequence number to decide.
  // Compare the difference to support integer roll-over.
  return (sequence_num - other.sequence_num) > 0;
}

Message::~Message() = default;

} // namespace base
} // namespace media
