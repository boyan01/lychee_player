//
// Created by boyan on 2021/2/12.
//

#ifndef FFPLAYER_FFP_DEFINE_H
#define FFPLAYER_FFP_DEFINE_H

#include <memory>

template <typename T>
using unique_ptr_d = std::unique_ptr<T, void (*)(T* ptr)>;

#define CHECK_VALUE_WITH_RETURN(VALUE, RETURN)                             \
  if (!(VALUE)) {                                                          \
    av_log(nullptr, AV_LOG_ERROR, "check %s value failed in %s\n", #VALUE, \
           __FUNCTION__);                                                  \
    return RETURN;                                                         \
  }

#define CHECK_VALUE(VALUE)                                                 \
  if (!(VALUE)) {                                                          \
    av_log(nullptr, AV_LOG_ERROR, "check %s value failed in %s\n", #VALUE, \
           __FUNCTION__);                                                  \
    return;                                                                \
  }

#ifdef _WIN32
#define FFPLAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define FFPLAYER_EXPORT \
  extern "C" __attribute__((visibility("default"))) __attribute__((used))
#endif

#endif  // FFPLAYER_FFP_DEFINE_H
