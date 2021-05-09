//
// Created by yangbin on 2021/5/7.
//

#ifndef MEDIA_BASE_BASE_CALLBACK_H_
#define MEDIA_BASE_BASE_CALLBACK_H_

namespace media {

template<typename Signature>
class OnceCallback;

/**
 * A callback which can only be called once.
 */
template<typename Result, typename ...Args>
class OnceCallback<Result(Args...)> {

 public:

  explicit OnceCallback(std::function<Result(Args...)> function) : function_(std::move(function)) {

  }
  OnceCallback() = default;
  OnceCallback(OnceCallback &&) noexcept = default;
  OnceCallback(const OnceCallback &) noexcept = delete;

  OnceCallback &operator=(OnceCallback &&) noexcept = default;
  OnceCallback &operator=(const OnceCallback &) noexcept = delete;

  void Reset() {
    function_ = nullptr;
  }

  bool is_null() const { return !function_; }

  explicit operator bool() const { return !is_null(); }

  void operator()(Args...) const &{
    static_assert(!sizeof(*this),
                  "OnceCallback::() may only be invoked on a non-const "
                  "rvalue, i.e. std::move(callback).Run().");
  }

  void operator()(Args... args) &&{
    OnceCallback cb = std::move(*this);
    auto f = cb.function_;
    f(std::forward<Args>(args)...);
  }

 private:
  std::function<Result(Args...)> function_;

};

//template<typename Result, typename ...Args>
//OnceCallback<Result(Args...)> BindOnce(std::function<Result(Args...)> function, Args... args) {
//  std::function<Result(Args...)> bind = std::bind(function, std::forward<Args>(args)...);
//  OnceCallback<Result(Args...)> callback(bind);
//  return std::move(callback);
//}
//

} // namespace media


#endif //MEDIA_BASE_BASE_CALLBACK_H_
