//
// Created by boyan on 2021/3/27.
//

#include "message.h"

#include <utility>
#include "location.h"

namespace media {
namespace base {

Message::Message(TaskClosure task,
                 const media::tracked_objects::Location &posted_from,
                 const std::chrono::milliseconds &delayed_run_time)
    : task(std::move(task)),
      posted_from(posted_from),
      when(std::chrono::system_clock::now() + delayed_run_time),
      sequence_num(0) {

}

Message::Message(TaskClosure task, const tracked_objects::Location &posted_from)
    : task(std::move(task)),
      posted_from(posted_from),
      sequence_num(0),
      when(std::chrono::system_clock::now()) {

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
