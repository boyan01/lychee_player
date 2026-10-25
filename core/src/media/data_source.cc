#include "media/data_source.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include "backends/apple/http_data_source.h"
#endif

namespace lychee {

namespace {

class FileDataSource : public DataSource {
 public:
  FileDataSource(std::FILE* file, int64_t size) : file_(file), size_(size) {}
  ~FileDataSource() override { std::fclose(file_); }

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (fseeko(file_, offset, SEEK_SET) != 0) {
      error_ = std::strerror(errno);
      return -1;
    }
    size_t read = std::fread(data, 1, size, file_);
    if (read == 0 && std::ferror(file_)) {
      error_ = std::strerror(errno);
      std::clearerr(file_);
      return -1;
    }
    return static_cast<int64_t>(read);
  }

  int64_t size() const override { return size_; }
  std::string error() const override { return error_; }

 private:
  std::FILE* file_;
  const int64_t size_;
  std::string error_;
};

}  // namespace

std::unique_ptr<DataSource> OpenDataSource(const std::string& url,
                                           std::string* error) {
  std::string path = url;
  size_t scheme_end = url.find("://");
  if (scheme_end != std::string::npos) {
    std::string scheme = url.substr(0, scheme_end);
    if (scheme == "http" || scheme == "https") {
#if defined(__APPLE__)
      return HttpDataSource::Open(url, error);
#else
      *error = "no HTTP client in this build";
      return nullptr;
#endif
    }
    if (scheme != "file") {
      *error = "unsupported URL scheme: " + scheme;
      return nullptr;
    }
    path = url.substr(scheme_end + 3);
  }
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (!file) {
    *error = path + ": " + std::strerror(errno);
    return nullptr;
  }
  fseeko(file, 0, SEEK_END);
  int64_t size = ftello(file);
  return std::make_unique<FileDataSource>(file, size);
}

}  // namespace lychee
