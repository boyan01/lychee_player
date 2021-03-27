//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_MESSAGE_QUEUE_H_
#define MEDIA_BASE_MESSAGE_QUEUE_H_

#include <functional>

#include "location.h"

namespace media {
namespace base {

class MessageQueue {

  void PostTask(const tracked_objects::Location &from_here, const std::function<void(void)> &task);

};

} // namespace base
} // namespace media

#endif //MEDIA_BASE_MESSAGE_QUEUE_H_
