#ifndef LYCHEE_BACKENDS_WINDOWS_HTTP_DATA_SOURCE_H_
#define LYCHEE_BACKENDS_WINDOWS_HTTP_DATA_SOURCE_H_

#include <memory>
#include <string>
#include <thread>

#include "media/data_source.h"

namespace lychee {

struct WinHttpState;

// http(s) through WinHTTP (Schannel, the system's proxy settings), as
// Apple's HttpDataSource does it with NSURLSession: a thread streams one
// request from the offset read into a buffer, a few MB ahead of the
// reader, and a read elsewhere replaces the request with a range request.
class WinHttpDataSource : public DataSource {
 public:
  // Starts the request; errors (no such host, HTTP 404) surface on the
  // first Read().
  static std::unique_ptr<DataSource> Open(const std::string& url,
                                          std::string* error);
  ~WinHttpDataSource() override;

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override;
  int64_t size() const override;
  bool seekable() const override;
  std::string url() const override;
  void Interrupt() override;
  void ClearInterrupt() override;
  bool interrupted() const override;
  std::string error() const override;

 private:
  explicit WinHttpDataSource(std::unique_ptr<WinHttpState> state);

  std::unique_ptr<WinHttpState> state_;
  std::thread thread_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_WINDOWS_HTTP_DATA_SOURCE_H_
