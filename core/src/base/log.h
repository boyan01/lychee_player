#ifndef LYCHEE_BASE_LOG_H_
#define LYCHEE_BASE_LOG_H_

namespace lychee {

enum class LogLevel { kError = 0, kWarning, kInfo, kDebug, kVerbose };

using LogSink = void (*)(void* user_data, int level, const char* message);

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();
void SetLogSink(LogSink sink, void* user_data);

#if defined(__GNUC__)
__attribute__((format(printf, 3, 4)))
#endif
void LogMessage(LogLevel level, const char* tag, const char* format, ...);

}  // namespace lychee

#ifndef LOG_TAG
#define LOG_TAG "lychee"
#endif

#define LYCHEE_LOG(level, ...)                                         \
  do {                                                                 \
    if (::lychee::GetLogLevel() >= ::lychee::LogLevel::level)          \
      ::lychee::LogMessage(::lychee::LogLevel::level, LOG_TAG,         \
                           __VA_ARGS__);                               \
  } while (0)

#define LOGE(...) LYCHEE_LOG(kError, __VA_ARGS__)
#define LOGW(...) LYCHEE_LOG(kWarning, __VA_ARGS__)
#define LOGI(...) LYCHEE_LOG(kInfo, __VA_ARGS__)
#define LOGD(...) LYCHEE_LOG(kDebug, __VA_ARGS__)
#define LOGV(...) LYCHEE_LOG(kVerbose, __VA_ARGS__)

#endif  // LYCHEE_BASE_LOG_H_
