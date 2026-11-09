#ifndef LYCHEE_MEDIA_ERROR_H_
#define LYCHEE_MEDIA_ERROR_H_

#include <cstdlib>
#include <string>
#include <string_view>

namespace lychee {

// What an error is about, for the caller to decide what to do next
// (lychee_error_kind, the same values).
enum class ErrorKind {
  kNone = 0,
  kOther = 1,
  kNetwork = 2,
  kHttp = 3,
  kFile = 4,
  kFormat = 5,
  kCodec = 6,
  kDecode = 7,
  kEndedEarly = 8,
};

struct ErrorCode {
  ErrorKind kind = ErrorKind::kNone;
  // The response's status for kHttp, 0 if the client did not say which.
  int http_status = 0;
};

// Error messages travel as strings (std::string* error, error()), through
// wrappers that add context ("segment 3: " + inner). The kind travels in
// them as a tag, "\x1f" KIND ":" STATUS "\x1f", so that no signature has
// to change and a wrapper keeps it by keeping the text. The first tag in
// a message wins: the innermost cause, unless a wrapper puts its own
// before it. The player takes the tags out (TakeErrorCode()) before a
// message leaves the library, and the log drops them.
inline constexpr char kErrorTagMark = '\x1f';

inline std::string ErrorTag(ErrorKind kind, int http_status = 0) {
  return kErrorTagMark + std::to_string(static_cast<int>(kind)) + ":" +
         std::to_string(http_status) + kErrorTagMark;
}

// |message| with the kind in front, unless it already has one.
inline std::string TagError(ErrorKind kind,
                            std::string message,
                            int http_status = 0) {
  if (message.find(kErrorTagMark) != std::string::npos) {
    return message;
  }
  return ErrorTag(kind, http_status) + message;
}

// The first tag in |message|, or "": to carry a cause's kind into a
// message of its own (ErrorTagOf(source->error()) + "input ended ...").
inline std::string ErrorTagOf(std::string_view message) {
  const size_t start = message.find(kErrorTagMark);
  if (start == std::string_view::npos) {
    return "";
  }
  const size_t end = message.find(kErrorTagMark, start + 1);
  if (end == std::string_view::npos) {
    return "";
  }
  return std::string(message.substr(start, end - start + 1));
}

// Removes the tags from |message| and returns the first one's kind:
// kOther for a message with none, kNone for an empty one.
inline ErrorCode TakeErrorCode(std::string* message) {
  ErrorCode code;
  std::string text;
  size_t at = 0;
  while (at < message->size()) {
    const size_t start = message->find(kErrorTagMark, at);
    const size_t end = start == std::string::npos
                           ? std::string::npos
                           : message->find(kErrorTagMark, start + 1);
    if (end == std::string::npos) {
      text.append(*message, at, std::string::npos);
      break;
    }
    text.append(*message, at, start - at);
    if (code.kind == ErrorKind::kNone) {
      const char* tag = message->c_str() + start + 1;
      char* colon = nullptr;
      const long kind = std::strtol(tag, &colon, 10);
      if (*colon == ':' && kind > 0 &&
          kind <= static_cast<long>(ErrorKind::kEndedEarly)) {
        code.kind = static_cast<ErrorKind>(kind);
        code.http_status =
            static_cast<int>(std::strtol(colon + 1, nullptr, 10));
      }
    }
    at = end + 1;
  }
  *message = std::move(text);
  if (code.kind == ErrorKind::kNone && !message->empty()) {
    code.kind = ErrorKind::kOther;
  }
  return code;
}

inline std::string WithoutErrorTags(std::string message) {
  TakeErrorCode(&message);
  return message;
}

}  // namespace lychee

#endif  // LYCHEE_MEDIA_ERROR_H_
