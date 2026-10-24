#ifndef LYCHEE_BASE_THREAD_NAME_H_
#define LYCHEE_BASE_THREAD_NAME_H_

#include <pthread.h>

namespace lychee {

// Names the calling thread, so debuggers, `sample` and leak checks can tell
// the library's threads apart. Names are at most 15 characters on Linux.
inline void SetCurrentThreadName(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__linux__)
  pthread_setname_np(pthread_self(), name);
#else
  (void)name;
#endif
}

}  // namespace lychee

#endif  // LYCHEE_BASE_THREAD_NAME_H_
