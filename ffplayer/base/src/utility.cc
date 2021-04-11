//
// Created by yangbin on 2021/3/28.
//

#include "base/utility.h"

#if WIN32
#include <Windows.h>
#include <processthreadsapi.h>
#include <string>
#else
#include "pthread.h"
#endif

namespace media {
namespace utility {

void update_thread_name(const char *name) {
#if __linux__ || __ANDROID__
  pthread_setname_np(pthread_self(), name);
#elif WIN32
  int ret = MultiByteToWideChar(CP_UTF8, 0, name, (int) strlen(name), nullptr, 0);
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

}
}
