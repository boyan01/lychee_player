//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_
#define MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_

#include <utility>

#include "functional"

#include "base/message_loop.h"
#include "base/callback.h"

namespace media {

template<typename... Args>
std::function<void(Args...)> BindToLoop(base::MessageLoop *loop, std::function<void(Args...)> cb) {
  std::function<void(Args...)> func = [loop, callback(std::move(cb))](Args... args) {
    loop->PostTask(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("BindToLoop"),
        std::bind(callback, std::forward<Args>(args)...));
  };
  return func;
}

template<typename Args = void>
std::function<void(void)> BindToLoop(base::MessageLoop *loop, std::function<void(void)> cb) {
  DCHECK(loop);
  std::function<void(void)> func = [loop, callback(std::move(cb))]() {
    loop->PostTask(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("BindToLoop"),
        callback);
  };
  return func;
}

template<typename... Args>
std::function<void(Args...)> BindToCurrentLoop(std::function<void(Args...)> cb) {
  return BindToLoop(base::MessageLoop::current(), std::move(cb));
}

}

#endif //MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_
