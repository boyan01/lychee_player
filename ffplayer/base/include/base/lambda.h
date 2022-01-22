//
// Created by yangbin on 2021/4/7.
//

#ifndef MEDIA_BASE_LAMBDA_H_
#define MEDIA_BASE_LAMBDA_H_

#include "basictypes.h"
#include "functional"

namespace media {

#define WEAK_THIS(TYPE) weak_this(std::weak_ptr<TYPE>(shared_from_this()))

template <class Class, class... Args>
class WeakBind0 {
 public:
  WeakBind0(const std::weak_ptr<Class> &weak_ptr,
            const std::function<void(Class *, Args...)> &callback)
      : callback_(callback), weak_ptr_(weak_ptr) {}

  void operator()(Args &&...args) {
    if (auto ptr = weak_ptr_.lock()) {
      callback_(ptr.get(), std::forward<Args>(args)...);
    }
  }

 private:
  std::function<void(Class *, Args...)> callback_;
  std::weak_ptr<Class> weak_ptr_;
};

template <class Result, class Class, class... Args>
class WeakBind {
 public:
  WeakBind(const std::function<Result(Class *, Args...)> &callback,
           const std::weak_ptr<Class> &weak_ptr, Result default_value)
      : callback_(callback),
        weak_ptr_(weak_ptr),
        default_value_(default_value) {}

  Result operator()(Args &&...args) {
    if (auto ptr = weak_ptr_.lock()) {
      callback_(ptr.get(), std::forward<Args>(args)...);
    } else {
      return default_value_;
    }
  }

 private:
  std::function<Result(Class *, Args...)> callback_;
  std::weak_ptr<Class> weak_ptr_;
  Result default_value_;
};

template <class Class, class... Args>
std::function<void(Args...)> bind_weak(void (Class::*callback)(Args...),
                                       std::shared_ptr<Class> ptr) {
  return WeakBind0<Class, Args...>(ptr, callback);
}

template <class Result, class Class, class... Args>
WeakBind<Result, Class, Args...> bind_weak(Result (Class::*callback)(Args...),
                                           std::shared_ptr<Class> ptr,
                                           Result default_value) {
  return WeakBind<Result, Class, Args...>(callback, ptr, default_value);
}

}  // namespace media

#endif  // MEDIA_BASE_LAMBDA_H_
