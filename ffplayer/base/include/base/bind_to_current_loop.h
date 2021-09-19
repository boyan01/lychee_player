//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_
#define MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_

#include <utility>

#include "functional"

#include "base/message_loop.h"
#include "base/callback.h"
#include "base/task_runner.h"

namespace media {

template<typename... Args>
std::function<void(Args...)> BindToLoop(
    const std::shared_ptr<base::MessageLooper> &loop,
    std::function<void(Args...)> cb
) {
  DCHECK(loop);
  std::function<void(Args...)> func = [loop, callback(std::move(cb))](Args... args) {
    loop->PostTask(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("BindToLoop"),
        std::bind(callback, std::forward<Args>(args)...));
  };
  return func;
}

// FIXME unsafe
template<typename... Args>
std::function<void(Args...)> BindToRunner(TaskRunner *runner, std::function<void(Args...)> cb) {
  std::function<void(Args...)> func = [runner, callback(std::move(cb))](Args... args) {
    runner->PostTask(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("BindToLoop"),
        std::bind(callback, std::forward<Args>(args)...));
  };
  return func;
}

template<typename Args = void>
std::function<void(void)> BindToLoop(const std::shared_ptr<base::MessageLooper> &loop, std::function<void(void)> cb) {
  DCHECK(loop);
  std::function<void(void)> func = [loop, callback(std::move(cb))]() {
    DCHECK(loop);
    loop->PostTask(
        FROM_HERE_WITH_EXPLICIT_FUNCTION("BindToLoop"),
        callback);
  };
  return func;
}

template<typename... Args>
std::function<void(Args...)> BindToCurrentLoop(std::function<void(Args...)> cb) {
  return BindToLoop(base::MessageLooper::Current(), std::move(cb));
}

template<typename Args = void>
std::function<void(void)> BindToCurrentLoop(std::function<void(void)> cb) {
  return BindToLoop(base::MessageLooper::Current(), std::move(cb));
}

}

#endif //MEDIA_BASE_BIND_TO_CURRENT_LOOP_H_
