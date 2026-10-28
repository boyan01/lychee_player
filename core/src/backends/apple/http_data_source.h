#ifndef LYCHEE_BACKENDS_APPLE_HTTP_DATA_SOURCE_H_
#define LYCHEE_BACKENDS_APPLE_HTTP_DATA_SOURCE_H_

#include <memory>
#include <string>

#include "media/data_source.h"

namespace lychee {

struct HttpState;
class HttpSession;

// An NSURLSession for HttpDataSources to share, so that their requests
// reuse its connections (kept alive) instead of each making its own. The
// sources keep it alive.
std::shared_ptr<HttpSession> CreateHttpSession();

// http(s) through NSURLSession: one streaming request from the offset
// read, replaced by a range request when a read jumps elsewhere. Buffers
// a few MB ahead of the reader and pauses the request beyond that.
class HttpDataSource : public DataSource {
 public:
  // Starts the request in |session|, or in a session of its own; errors
  // (no such host, HTTP 404) surface on the first Read().
  static std::unique_ptr<DataSource> Open(
      const std::string& url,
      std::string* error,
      std::shared_ptr<HttpSession> session = nullptr);
  ~HttpDataSource() override;

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override;
  int64_t size() const override;
  bool seekable() const override;
  std::string url() const override;
  bool cleartext_refused() const override;
  void Interrupt() override;
  void ClearInterrupt() override;
  bool interrupted() const override;
  std::string error() const override;

 private:
  explicit HttpDataSource(std::shared_ptr<HttpState> state);

  std::shared_ptr<HttpState> state_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_HTTP_DATA_SOURCE_H_
