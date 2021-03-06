//
// Created by yangbin on 2021/2/13.
//

#include "ffp_utils.h"

#if WIN32

#include <windows.h>
#include <processthreadsapi.h>
#include <string>

#else
#include "pthread.h"
#endif

extern "C" {
#include "libavutil/avutil.h"
}

void update_thread_name(const char *name) {
#if __LINUX__ || __ANDROID__
  pthread_setname_np(pthread_self(), name);
#elif WIN32
  int ret = MultiByteToWideChar(CP_UTF8, 0, name, strlen(name), nullptr, 0);
  if (ret < 0) {
    return;
  }
  std::wstring wide_name;
  wide_name.resize(ret + 10);

  if (MultiByteToWideChar(CP_UTF8, 0, name, (int) strlen(name), &wide_name[0], (int) wide_name.size()) < 0) {
    return;
  }
  SetThreadDescription(GetCurrentThread(), wide_name.data());
#else
  pthread_setname_np(name);
#endif

}

const char *av_err_to_str(int errnum) {
  static char av_error[AV_ERROR_MAX_STRING_SIZE] = {0};
  return av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum);
}
