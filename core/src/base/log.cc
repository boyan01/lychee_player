#include "base/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace lychee {

namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::kInfo)};
std::mutex g_sink_mutex;
LogSink g_sink = nullptr;
void* g_sink_user_data = nullptr;

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
  std::lock_guard<std::mutex> lock(g_sink_mutex);
  g_sink = sink;
  g_sink_user_data = user_data;
}

void LogMessage(LogLevel level, const char* tag, const char* format, ...) {
  char message[1024];
  int prefix = snprintf(message, sizeof(message), "[%s/%s] ", LevelName(level),
                        tag);
  va_list args;
  va_start(args, format);
  vsnprintf(message + prefix, sizeof(message) - prefix, format, args);
  va_end(args);

  std::lock_guard<std::mutex> lock(g_sink_mutex);
  if (g_sink) {
    g_sink(g_sink_user_data, static_cast<int>(level), message);
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
