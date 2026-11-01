#include "media/text_encoding.h"

#include <iconv.h>

#include <cerrno>
#include <cstdint>

namespace lychee {

bool ConvertToUtf8(std::string_view data,
                   const std::string& encoding,
                   std::string* text) {
  iconv_t converter = iconv_open("UTF-8", encoding.c_str());
  // Fails with (iconv_t)-1.
  if (reinterpret_cast<intptr_t>(converter) == -1) {
    return false;
  }
  text->clear();
  // iconv's input is char*, though it never writes through it.
  char* in = const_cast<char*>(data.data());
  size_t in_left = data.size();
  char buffer[4096];
  while (in_left > 0) {
    char* out = buffer;
    size_t out_left = sizeof(buffer);
    const size_t result = iconv(converter, &in, &in_left, &out, &out_left);
    text->append(buffer, static_cast<size_t>(out - buffer));
    if (result != static_cast<size_t>(-1) || errno == E2BIG) {
      continue;
    }
    // EILSEQ: a byte the encoding cannot read; EINVAL: a sequence cut
    // off at the end. Either way, one replacement and on.
    text->append("\xEF\xBF\xBD");
    ++in;
    --in_left;
  }
  iconv_close(converter);
  return true;
}

}  // namespace lychee
