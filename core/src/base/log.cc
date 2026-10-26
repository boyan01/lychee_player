#include "base/log.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace lychee {

namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::kInfo)};
// The sink is called without the lock, so it may log, replace itself or
// call into the library. SetLogSink() waits for the calls in flight, so
// once it returns the old sink's user data may go.
std::mutex g_sink_mutex;
std::condition_variable g_sink_idle;
LogSink g_sink = nullptr;
void* g_sink_user_data = nullptr;
int g_sink_calls = 0;
thread_local bool t_in_sink = false;

const char* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kError:
      return "E";
    case LogLevel::kWarning:
      return "W";
    case LogLevel::kInfo:
      return "I";
    case LogLevel::kDebug:
      return "D";
    case LogLevel::kVerbose:
      return "V";
  }
  return "?";
}

}  // namespace

void SetLogLevel(LogLevel level) {
  g_level = static_cast<int>(level);
}

LogLevel GetLogLevel() {
  return static_cast<LogLevel>(g_level.load());
}

void SetLogSink(LogSink sink, void* user_data) {
  std::unique_lock<std::mutex> lock(g_sink_mutex);
  g_sink = sink;
  g_sink_user_data = user_data;
  // Not for the sink's own call, when it replaces itself.
  const int own = t_in_sink ? 1 : 0;
  g_sink_idle.wait(lock, [own] { return g_sink_calls == own; });
}

void LogMessage(LogLevel level, const char* tag, const char* format, ...) {
  char message[1024];
  int prefix = snprintf(message, sizeof(message), "[%s/%s] ", LevelName(level),
                        tag);
  va_list args;
  va_start(args, format);
  vsnprintf(message + prefix, sizeof(message) - prefix, format, args);
  va_end(args);

  LogSink sink = nullptr;
  void* user_data = nullptr;
  // What the sink logs itself goes to stderr: it could log forever.
  if (!t_in_sink) {
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    sink = g_sink;
    user_data = g_sink_user_data;
    g_sink_calls += sink ? 1 : 0;
  }
  if (sink) {
    t_in_sink = true;
    sink(user_data, static_cast<int>(level), message);
    t_in_sink = false;
    {
      std::lock_guard<std::mutex> lock(g_sink_mutex);
      g_sink_calls--;
    }
    g_sink_idle.notify_all();
  } else {
#if defined(__ANDROID__)
    // stderr goes nowhere in an app.
    __android_log_write(ANDROID_LOG_INFO, "lychee", message);
#else
    fprintf(stderr, "%s\n", message);
#endif
  }
}

}  // namespace lychee
