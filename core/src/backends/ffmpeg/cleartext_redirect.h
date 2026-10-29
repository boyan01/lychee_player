#ifndef LYCHEE_BACKENDS_FFMPEG_CLEARTEXT_REDIRECT_H_
#define LYCHEE_BACKENDS_FFMPEG_CLEARTEXT_REDIRECT_H_

#include <string>

struct AVIOInterruptCB;

namespace lychee {

// Before FFmpeg's cleartext fallback, find an HTTPS redirect without opening
// TLS. Returns its URL, or empty if FFmpeg should read HTTP as before. A
// nonempty error instead stops the open (invalid/too many redirects).
std::string FindHttpsRedirect(const std::string& url,
                              const AVIOInterruptCB& interrupt,
                              std::string* error);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_CLEARTEXT_REDIRECT_H_
