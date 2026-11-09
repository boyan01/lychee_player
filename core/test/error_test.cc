#include "media/error.h"

#include <string>

#include "test.h"

namespace lychee {
namespace {

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(ErrorCode, TakesTheFirstTagAndRemovesAll) {
  // A wrapper adds context around the cause, and a kind of its own after it.
  std::string message =
      "segment 3: " + TagError(ErrorKind::kHttp, "HTTP 410 Gone", 410) + " (" +
      ErrorTag(ErrorKind::kEndedEarly) + "input ended)";
  const ErrorCode code = TakeErrorCode(&message);
  EXPECT_EQ(static_cast<int>(code.kind), static_cast<int>(ErrorKind::kHttp));
  EXPECT_EQ(code.http_status, 410);
  EXPECT_EQ(message, std::string("segment 3: HTTP 410 Gone (input ended)"));
}

TEST(ErrorCode, UntaggedIsOtherAndEmptyIsNone) {
  std::string message = "seek failed";
  EXPECT_EQ(static_cast<int>(TakeErrorCode(&message).kind),
            static_cast<int>(ErrorKind::kOther));
  EXPECT_EQ(message, std::string("seek failed"));
  message.clear();
  EXPECT_EQ(static_cast<int>(TakeErrorCode(&message).kind),
            static_cast<int>(ErrorKind::kNone));
}

TEST(ErrorCode, KeepsTheCauseKind) {
  // TagError() does not replace a cause's kind; ErrorTagOf() carries it.
  const std::string cause = TagError(ErrorKind::kNetwork, "reset");
  std::string message = TagError(ErrorKind::kFormat, "read: " + cause);
  EXPECT_EQ(static_cast<int>(TakeErrorCode(&message).kind),
            static_cast<int>(ErrorKind::kNetwork));
  message = ErrorTagOf(cause) + ErrorTag(ErrorKind::kEndedEarly) + "ended";
  EXPECT_EQ(static_cast<int>(TakeErrorCode(&message).kind),
            static_cast<int>(ErrorKind::kNetwork));
  EXPECT_EQ(message, std::string("ended"));
  EXPECT_EQ(ErrorTagOf("no tag"), std::string());
}

TEST(ErrorCode, IgnoresAStrayMark) {
  std::string message = "odd \x1f text";
  EXPECT_EQ(static_cast<int>(TakeErrorCode(&message).kind),
            static_cast<int>(ErrorKind::kOther));
  EXPECT_EQ(message, std::string("odd \x1f text"));
  message =
      "\x1f"
      "99:0\x1f"
      "bad kind";
  EXPECT_EQ(static_cast<int>(TakeErrorCode(&message).kind),
            static_cast<int>(ErrorKind::kOther));
  EXPECT_EQ(message, std::string("bad kind"));
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
