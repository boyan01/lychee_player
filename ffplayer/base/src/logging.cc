//
// Created by boyan on 2021/3/27.
//

#include <iomanip>
#include "base/logging.h"

namespace logging {

// For LOG_ERROR and above, always print to stderr.
const int kAlwaysPrintErrorLevel = LOG_ERROR;

const char *const log_severity_names[LOG_NUM_SEVERITIES] = {
    "INFO", "WARNING", "ERROR", "ERROR_REPORT", "FATAL"};

int min_log_level = 0;

// The default set here for logging_destination will only be used if
// InitLogging is not called.  On Windows, use a file next to the exe;
// on POSIX platforms, where it may not even be possible to locate the
// executable on disk, use stderr.
LoggingDestination logging_destination = LOG_ONLY_TO_SYSTEM_DEBUG_LOG;

LogMessage::LogMessage(const char *file, int line, LogSeverity severity,
                       int ctr)
    : severity_(severity), file_(file), line_(line) {
  Init(file, line);
}

LogMessage::LogMessage(const char *file, int line)
    : severity_(LOG_INFO), file_(file), line_(line) {
  Init(file, line);
}

LogMessage::LogMessage(const char *file, int line, LogSeverity severity)
    : severity_(severity), file_(file), line_(line) {
  Init(file, line);
}

LogMessage::LogMessage(const char *file, int line, std::string *result)
    : severity_(LOG_FATAL), file_(file), line_(line) {
  Init(file, line);
  stream_ << "Check failed: " << *result;
  delete result;
}

LogMessage::LogMessage(const char *file, int line, LogSeverity severity,
                       std::string *result)
    : severity_(severity), file_(file), line_(line) {
  Init(file, line);
  stream_ << "Check failed: " << *result;
  delete result;
}

LogMessage::~LogMessage() {
  // TODO(port): enable stacktrace generation on LOG_FATAL once backtrace are
  // working in Android.
#if  !defined(NDEBUG) && !defined(OS_ANDROID) && !defined(OS_NACL)
  if (severity_ == LOG_FATAL) {
    // add stack trace to stream.
  }
#endif
  stream_ << std::endl;
  std::string str_newline(stream_.str());

  if (logging_destination == LOG_ONLY_TO_SYSTEM_DEBUG_LOG ||
      logging_destination == LOG_TO_BOTH_FILE_AND_SYSTEM_DEBUG_LOG) {
#if defined(OS_WIN)
    OutputDebugStringA(str_newline.c_str());
#elif defined(OS_ANDROID)
    android_LogPriority priority = ANDROID_LOG_UNKNOWN;
    switch (severity_) {
      case LOG_INFO:
        priority = ANDROID_LOG_INFO;
        break;
      case LOG_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
      case LOG_ERROR:
      case LOG_ERROR_REPORT:
        priority = ANDROID_LOG_ERROR;
        break;
      case LOG_FATAL:
        priority = ANDROID_LOG_FATAL;
        break;
    }
    __android_log_write(priority, "chromium", str_newline.c_str());
#endif
    fprintf(stderr, "%s", str_newline.c_str());
    fflush(stderr);
  } else if (severity_ >= kAlwaysPrintErrorLevel) {
    // When we're only outputting to a log file, above a certain log level, we
    // should still output to stderr so that we can better detect and diagnose
    // problems with unit tests, especially on the buildbots.
    fprintf(stderr, "%s", str_newline.c_str());
    fflush(stderr);

  }

}

// writes the common header info to the stream
void LogMessage::Init(const char *file, int line) {
  std::string filename(file);
  size_t last_slash_pos = filename.find_last_of("\\/");
  if (last_slash_pos != std::string::npos) {
    filename = filename.substr(last_slash_pos + 1);
  }

  // TODO: It might be nice if the columns were fixed width.

  stream_ << '[';

  // add timestamp.
  {
    time_t t = time(nullptr);
    struct tm local_time = {0};
#if _MSC_VER >= 1400
    localtime_s(&local_time, &t);
#else
    localtime_r(&t, &local_time);
#endif
    struct tm *tm_time = &local_time;
    stream_ << std::setfill('0')
            << std::setw(2) << 1 + tm_time->tm_mon
            << std::setw(2) << tm_time->tm_mday
            << '/'
            << std::setw(2) << tm_time->tm_hour
            << std::setw(2) << tm_time->tm_min
            << std::setw(2) << tm_time->tm_sec
            << ':';
  }
  if (severity_ >= 0)
    stream_ << log_severity_names[severity_];
  else
    stream_ << "VERBOSE" << -severity_;

  stream_ << ":" << filename << "(" << line << ")] ";

}

int GetMinLogLevel() {
  return min_log_level;
}

}