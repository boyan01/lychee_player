#ifndef LYCHEE_BACKENDS_FFMPEG_CLEARTEXT_REDIRECT_H_
#define LYCHEE_BACKENDS_FFMPEG_CLEARTEXT_REDIRECT_H_

#include <mutex>
#include <set>
#include <string>

struct AVIOInterruptCB;

namespace lychee {

// Before FFmpeg's cleartext fallback, finds an HTTPS redirect without
// opening TLS. Each probe costs a request, so an origin whose URL did not
// redirect is remembered and left to FFmpeg until FFmpeg fails on it.
class CleartextRedirects {
 public:
  // The HTTPS URL |url| redirects to, or empty if FFmpeg should read HTTP
  // as before. A nonempty error instead stops the open (invalid or too
  // many redirects).
  std::string Find(const std::string& url,
                   const AVIOInterruptCB& interrupt,
                   std::string* error);
  // Whether |url|'s origin did not redirect before; Forget() probes it
  // again.
  bool Known(const std::string& url);
  void Forget(const std::string& url);

 private:
  std::mutex mutex_;
  std::set<std::string> plain_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_CLEARTEXT_REDIRECT_H_
