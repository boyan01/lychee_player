#ifndef LYCHEE_MEDIA_TEXT_ENCODING_H_
#define LYCHEE_MEDIA_TEXT_ENCODING_H_

#include <string>
#include <string_view>

namespace lychee {

// Converts |data| in the legacy encoding |encoding| (an IANA name such as
// "GB18030", "Big5", "Shift_JIS", "windows-1252"; case does not matter)
// to UTF-8 through the platform's converter: iconv on Apple and Linux,
// java.nio.charset on Android (none without a Java VM), and
// MultiByteToWideChar on Windows. Bytes the encoding cannot read become
// U+FFFD. False if the platform does not know |encoding|.
bool ConvertToUtf8(std::string_view data,
                   const std::string& encoding,
                   std::string* text);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_TEXT_ENCODING_H_
