//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_
#define MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_

#include "functional"

#include "base/message_loop.h"

namespace media {

template<typename... Args>
std::function<void(Args...)> BindToCurrentLoop(std::function<void(Args...)> cb) {
  auto callback = [callback(std::move(cb))](Args... args) {
    base::MessageLoop::current()->PostTask(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("BindToCurrentLoop"),
        std::bind(callback, std::forward<Args>(args)...));
  };
  return callback;
}

}

#endif //MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_
