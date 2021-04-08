//
// Created by boyan on 2021/3/27.
//

#ifndef MEDIA_BASE_LOGGING_H_
#define MEDIA_BASE_LOGGING_H_

#include <sstream>

#include "basictypes.h"

namespace logging {

// Where to record logging output? A flat file and/or system debug log via
// OutputDebugString. Defaults on Windows to LOG_ONLY_TO_FILE, and on
// POSIX to LOG_ONLY_TO_SYSTEM_DEBUG_LOG (aka stderr).
enum LoggingDestination {
  LOG_NONE,
  LOG_ONLY_TO_FILE,
  LOG_ONLY_TO_SYSTEM_DEBUG_LOG,
  LOG_TO_BOTH_FILE_AND_SYSTEM_DEBUG_LOG
};

typedef int LogSeverity;
const LogSeverity LOG_VERBOSE = -1;  // This is level 1 verbosity
// Note: the log severities are used to index into the array of names,
// see log_severity_names.
const LogSeverity LOG_INFO = 0;
const LogSeverity LOG_WARNING = 1;
const LogSeverity LOG_ERROR = 2;
const LogSeverity LOG_ERROR_REPORT = 3;
const LogSeverity LOG_FATAL = 4;
const LogSeverity LOG_NUM_SEVERITIES = 5;

const LogSeverity LOG_DCHECK = LOG_FATAL;

// LOG_DFATAL is LOG_FATAL in debug mode, ERROR in normal mode
#ifdef NDEBUG
const LogSeverity LOG_DFATAL = LOG_ERROR;
#else
const LogSeverity LOG_DFATAL = LOG_FATAL;
#endif

// Gets the current log level.
int GetMinLogLevel();

// As special cases, we can assume that LOG_IS_ON(ERROR_REPORT) and
// LOG_IS_ON(FATAL) always hold.  Also, LOG_IS_ON(DFATAL) always holds
// in debug mode.  In particular, CHECK()s will always fire if they
// fail.
#define LOG_IS_ON(severity) \
  ((::logging::LOG_ ## severity) >= ::logging::GetMinLogLevel())

#define DCHECK_IS_ON() true
#define DLOG_IS_ON(severity) LOG_IS_ON(severity)

// This class more or less represents a particular log message.  You
// create an instance of LogMessage and then stream stuff to it.
// When you finish streaming to it, ~LogMessage is called and the
// full message gets streamed to the appropriate destination.
//
// You shouldn't actually use LogMessage's constructor to log things,
// though.  You should use the LOG() macro (and variants thereof)
// above.
class LogMessage {
 public:
  LogMessage(const char *file, int line, LogSeverity severity, int ctr);

  // Two special constructors that generate reduced amounts of code at
  // LOG call sites for common cases.
  //
  // Used for LOG(INFO): Implied are:
  // severity = LOG_INFO, ctr = 0
  //
  // Using this constructor instead of the more complex constructor above
  // saves a couple of bytes per call site.
  LogMessage(const char *file, int line);

  // Used for LOG(severity) where severity != INFO.  Implied
  // are: ctr = 0
  //
  // Using this constructor instead of the more complex constructor above
  // saves a couple of bytes per call site.
  LogMessage(const char *file, int line, LogSeverity severity);

  // A special constructor used for check failures.  Takes ownership
  // of the given string.
  // Implied severity = LOG_FATAL
  LogMessage(const char *file, int line, std::string *result);

  // A special constructor used for check failures, with the option to
  // specify severity.  Takes ownership of the given string.
  LogMessage(const char *file, int line, LogSeverity severity,
             std::string *result);

  ~LogMessage();

  std::ostream &stream() { return stream_; }

 private:
  void Init(const char *file, int line);

  LogSeverity severity_;
  std::ostringstream stream_;

  // The file and line information passed in to the constructor.
  const char *file_;
  const int line_;

#if defined(OS_WIN)
  // Stores the current value of GetLastError in the constructor and restores
// it in the destructor by calling SetLastError.
// This is useful since the LogMessage class uses a lot of Win32 calls
// that will lose the value of GLE and the code that called the log function
// will have lost the thread error value when the log call returns.
class SaveLastError {
 public:
  SaveLastError();
  ~SaveLastError();

  unsigned long get_error() const { return last_error_; }

 protected:
  unsigned long last_error_;
};

SaveLastError last_error_;
#endif

  DISALLOW_COPY_AND_ASSIGN(LogMessage);
};

// This class is used to explicitly ignore values in the conditional
// logging macros.  This avoids compiler warnings like "value computed
// is not used" and "statement has no effect".
class LogMessageVoid {
 public:
  LogMessageVoid() = default;
  // This has to be an operator with a precedence lower than << but
  // higher than ?:
  void operator&(std::ostream &) {}
};

#define COMPACT_GOOGLE_LOG_EX(ClassName, severity, ...) \
  logging::ClassName(__FILE__, __LINE__, (severity) , ##__VA_ARGS__)

#define COMPACT_GOOGLE_LOG_FATAL \
  COMPACT_GOOGLE_LOG_EX(LogMessage, logging::LOG_FATAL)

#define COMPACT_GOOGLE_LOG_ERROR \
  COMPACT_GOOGLE_LOG_EX(LogMessage, logging::LOG_ERROR)

#define COMPACT_GOOGLE_LOG_WARNING \
  COMPACT_GOOGLE_LOG_EX(LogMessage, logging::LOG_WARNING)

#define COMPACT_GOOGLE_LOG_INFO \
  COMPACT_GOOGLE_LOG_EX(LogMessage, logging::LOG_INFO)

#define COMPACT_GOOGLE_LOG_DCHECK COMPACT_GOOGLE_LOG_FATAL

// Helper macro which avoids evaluating the arguments to a stream if
// the condition doesn't hold.
#define LAZY_STREAM(stream, condition)                                  \
  !(condition) ? (void) 0 : ::logging::LogMessageVoid() & (stream)

// Build the error message string.  This is separate from the "Impl"
// function template because it is not performance critical and so can
// be out of line, while the "Impl" code should be inline.  Caller
// takes ownership of the returned string.
template<class t1, class t2>
std::string *MakeCheckOpString(const t1 &v1, const t2 &v2, const char *names) {
  std::ostringstream ss;
  ss << names << " (" << v1 << " vs. " << v2 << ")";
  auto *msg = new std::string(ss.str());
  return msg;
}

// Helper macro for binary operators.
// The 'switch' is used to prevent the 'else' from being ambiguous when the
// macro is used in an 'if' clause such as:
// if (a == 1)
//   CHECK_EQ(2, a);
#define CHECK_OP(name, op, val1, val2)                        \
  if (std::string* _result =                                  \
      logging::Check##name##Impl((val1), (val2),              \
                                 #val1 " " #op " " #val2))    \
    logging::LogMessage(                                      \
        __FILE__, __LINE__, ::logging::LOG_DCHECK,            \
        _result).stream()


// Helper functions for CHECK_OP macro.
// The (int, int) specialization works around the issue that the compiler
// will not instantiate the template version of the function on values of
// unnamed enum type - see comment below.
#define DEFINE_CHECK_OP_IMPL(name, op) \
  template <class t1, class t2> \
  inline std::string* Check##name##Impl(const t1& v1, const t2& v2, \
                                        const char* names) { \
    if (v1 op v2) return NULL; \
    else return MakeCheckOpString(v1, v2, names); \
  } \
  inline std::string* Check##name##Impl(int v1, int v2, const char* names) { \
    if (v1 op v2) return NULL; \
    else return MakeCheckOpString(v1, v2, names); \
  }
DEFINE_CHECK_OP_IMPL(EQ, ==)
DEFINE_CHECK_OP_IMPL(NE, !=)
DEFINE_CHECK_OP_IMPL(LE, <=)
DEFINE_CHECK_OP_IMPL(LT, <)
DEFINE_CHECK_OP_IMPL(GE, >=)
DEFINE_CHECK_OP_IMPL(GT, >)
#undef DEFINE_CHECK_OP_IMPL

#define CHECK_EQ(val1, val2) CHECK_OP(EQ, ==, val1, val2)
#define CHECK_NE(val1, val2) CHECK_OP(NE, !=, val1, val2)
#define CHECK_LE(val1, val2) CHECK_OP(LE, <=, val1, val2)
#define CHECK_LT(val1, val2) CHECK_OP(LT, < , val1, val2)
#define CHECK_GE(val1, val2) CHECK_OP(GE, >=, val1, val2)
#define CHECK_GT(val1, val2) CHECK_OP(GT, > , val1, val2)


// We use the preprocessor's merging operator, "##", so that, e.g.,
// LOG(INFO) becomes the token COMPACT_GOOGLE_LOG_INFO.  There's some funny
// subtle difference between ostream member streaming functions (e.g.,
// ostream::operator<<(int) and ostream non-member streaming functions
// (e.g., ::operator<<(ostream&, string&): it turns out that it's
// impossible to stream something like a string directly to an unnamed
// ostream. We employ a neat hack by calling the stream() member
// function of LogMessage which seems to avoid the problem.
#define LOG_STREAM(severity) COMPACT_GOOGLE_LOG_ ## severity.stream()

#define LOG(severity) LAZY_STREAM(LOG_STREAM(severity), LOG_IS_ON(severity))

#define DLOG(severity)                                          \
  LAZY_STREAM(LOG_STREAM(severity), DLOG_IS_ON(severity))

#define DCHECK(condition)                                           \
  LAZY_STREAM(LOG_STREAM(DCHECK), DCHECK_IS_ON() && !(condition))   \
  << "Check failed: " #condition ". "

#define CHECK(condition)                       \
  LAZY_STREAM(LOG_STREAM(FATAL), !(condition)) \
  << "Check failed: " #condition ". "

// Helper macro for binary operators.
// Don't use this macro directly in your code, use DCHECK_EQ et al below.
#define DCHECK_OP(name, op, val1, val2)                         \
  if (DCHECK_IS_ON())                                           \
    if (std::string* _result =                                  \
        logging::Check##name##Impl((val1), (val2),              \
                                   #val1 " " #op " " #val2))    \
      logging::LogMessage(                                      \
          __FILE__, __LINE__, ::logging::LOG_DCHECK,            \
          _result).stream()

// Equality/Inequality checks - compare two values, and log a
// LOG_DCHECK message including the two values when the result is not
// as expected.  The values must have operator<<(ostream, ...)
// defined.
//
// You may append to the error message like so:
//   DCHECK_NE(1, 2) << ": The world must be ending!";
//
// We are very careful to ensure that each argument is evaluated exactly
// once, and that anything which is legal to pass as a function argument is
// legal here.  In particular, the arguments may be temporary expressions
// which will end up being destroyed at the end of the apparent statement,
// for example:
//   DCHECK_EQ(string("abc")[1], 'b');
//
// WARNING: These may not compile correctly if one of the arguments is a pointer
// and the other is NULL. To work around this, simply static_cast NULL to the
// type of the desired pointer.

#define DCHECK_EQ(val1, val2) DCHECK_OP(EQ, ==, val1, val2)
#define DCHECK_NE(val1, val2) DCHECK_OP(NE, !=, val1, val2)
#define DCHECK_LE(val1, val2) DCHECK_OP(LE, <=, val1, val2)
#define DCHECK_LT(val1, val2) DCHECK_OP(LT, < , val1, val2)
#define DCHECK_GE(val1, val2) DCHECK_OP(GE, >=, val1, val2)
#define DCHECK_GT(val1, val2) DCHECK_OP(GT, > , val1, val2)

#define NOTREACHED() DCHECK(false)

// A non-macro interface to the log facility; (useful
// when the logging level is not a compile-time constant).
inline void LogAtLevel(int const log_level, std::string const &msg) {
  LogMessage(__FILE__, __LINE__, log_level).stream() << msg;
}

} // namespace logging

#endif //MEDIA_BASE_LOGGING_H_
