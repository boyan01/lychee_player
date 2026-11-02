#include "media/data_source.h"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(__APPLE__)
#include "backends/apple/http_data_source.h"
#elif defined(__ANDROID__)
#include "backends/android/http_data_source.h"
#elif defined(_WIN32)
#include <windows.h>

#include "backends/windows/http_data_source.h"
#elif defined(LYCHEE_WITH_FFMPEG)
#include "backends/ffmpeg/ffmpeg_data_source.h"
#endif

namespace lychee {

namespace {

#if defined(_WIN32)
int Seek(std::FILE* file, int64_t offset, int whence) {
  return _fseeki64(file, offset, whence);
}

int64_t Tell(std::FILE* file) {
  return _ftelli64(file);
}

std::string ErrorString(int error) {
  char text[128];
  strerror_s(text, sizeof(text), error);
  return text;
}

// Paths are UTF-8, as everywhere else in the API.
std::FILE* OpenFile(const std::string& path) {
  const int size =
      MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (size <= 0) {
    errno = EINVAL;
    return nullptr;
  }
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), size);
  std::FILE* file = nullptr;
  errno = _wfopen_s(&file, wide.c_str(), L"rb");
  return file;
}
#else
int Seek(std::FILE* file, int64_t offset, int whence) {
  return fseeko(file, offset, whence);
}

int64_t Tell(std::FILE* file) {
  return ftello(file);
}

std::string ErrorString(int error) {
  return std::strerror(error);
}

std::FILE* OpenFile(const std::string& path) {
  return std::fopen(path.c_str(), "rb");
}
#endif

class FileDataSource : public DataSource {
 public:
  FileDataSource(std::FILE* file, int64_t size) : file_(file), size_(size) {}
  ~FileDataSource() override { std::fclose(file_); }

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (Seek(file_, offset, SEEK_SET) != 0) {
      error_ = ErrorString(errno);
      return -1;
    }
    size_t read = std::fread(data, 1, size, file_);
    if (read == 0 && std::ferror(file_)) {
      error_ = ErrorString(errno);
      std::clearerr(file_);
      return -1;
    }
    return static_cast<int64_t>(read);
  }

  int64_t size() const override { return size_; }
  std::string error() const override { return error_; }
  bool reads_ahead() const override { return true; }

 private:
  std::FILE* file_;
  const int64_t size_;
  std::string error_;
};

class MemoryDataSource : public DataSource {
 public:
  explicit MemoryDataSource(std::vector<uint8_t> data)
      : data_(std::move(data)) {}

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (offset < 0) {
      return -1;
    }
    if (static_cast<uint64_t>(offset) >= data_.size()) {
      return 0;
    }
    size = std::min(size, data_.size() - static_cast<size_t>(offset));
    memcpy(data, data_.data() + offset, size);
    return static_cast<int64_t>(size);
  }

  int64_t size() const override { return static_cast<int64_t>(data_.size()); }
  std::string error() const override { return "read before the start"; }
  bool reads_ahead() const override { return true; }

 private:
  const std::vector<uint8_t> data_;
};

class PrefetchingDataSource : public DataSource {
 public:
  PrefetchingDataSource(std::unique_ptr<DataSource> source, size_t limit)
      : source_(std::move(source)), limit_(limit) {
    thread_ = std::thread([this] { Fill(); });
  }

  ~PrefetchingDataSource() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    source_->Interrupt();
    thread_.join();
  }

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] {
      return interrupted_ || done_ ||
             (offset >= 0 && static_cast<uint64_t>(offset) < buffer_.size());
    });
    if (interrupted_) {
      return -1;
    }
    if (offset >= 0 && static_cast<uint64_t>(offset) < buffer_.size()) {
      size = std::min(size, buffer_.size() - static_cast<size_t>(offset));
      memcpy(data, buffer_.data() + offset, size);
      return static_cast<int64_t>(size);
    }
    // The thread is done and touches |source_| no more.
    lock.unlock();
    return source_->Read(offset, data, size);
  }

  int64_t size() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ ? source_->size() : size_;
  }
  bool seekable() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ ? source_->seekable() : seekable_;
  }
  std::string url() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ ? source_->url() : url_;
  }
  bool cleartext_refused() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ ? source_->cleartext_refused() : cleartext_refused_;
  }
  std::string error() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ ? source_->error() : error_;
  }
  int64_t transfer_rate() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_ ? source_->transfer_rate() : transfer_rate_;
  }

  void Interrupt() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      interrupted_ = true;
    }
    condition_.notify_all();
    source_->Interrupt();
  }
  void ClearInterrupt() override {
    std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = false;
    source_->ClearInterrupt();
  }
  bool interrupted() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupted_;
  }

 private:
  void Fill() {
    std::vector<uint8_t> chunk(size_t{64} * 1024);
    size_t filled = 0;
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || filled >= limit_) {
          break;
        }
      }
      const int64_t n =
          source_->Read(static_cast<int64_t>(filled), chunk.data(),
                        std::min(chunk.size(), limit_ - filled));
      std::lock_guard<std::mutex> lock(mutex_);
      size_ = source_->size();
      seekable_ = source_->seekable();
      url_ = source_->url();
      cleartext_refused_ = source_->cleartext_refused();
      error_ = source_->error();
      transfer_rate_ = source_->transfer_rate();
      if (n <= 0) {
        break;
      }
      buffer_.insert(buffer_.end(), chunk.data(), chunk.data() + n);
      filled += static_cast<size_t>(n);
      condition_.notify_all();
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      done_ = true;
    }
    condition_.notify_all();
  }

  const std::unique_ptr<DataSource> source_;
  const size_t limit_;
  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<uint8_t> buffer_;
  bool stop_ = false;
  bool done_ = false;
  bool interrupted_ = false;
  // |source_|'s answers after the thread's last read.
  int64_t size_ = -1;
  bool seekable_ = true;
  std::string url_;
  bool cleartext_refused_ = false;
  std::string error_;
  int64_t transfer_rate_ = -1;
};

}  // namespace

std::unique_ptr<DataSource> CreateMemoryDataSource(std::vector<uint8_t> data) {
  return std::make_unique<MemoryDataSource>(std::move(data));
}

std::unique_ptr<DataSource> CreatePrefetchingDataSource(
    std::unique_ptr<DataSource> source,
    size_t limit) {
  return std::make_unique<PrefetchingDataSource>(std::move(source), limit);
}

DataSourceOpener CreateSharedOpener() {
#if defined(__APPLE__)
  std::shared_ptr<HttpSession> session = CreateHttpSession();
  return [session](const std::string& url, std::string* error) {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
      return HttpDataSource::Open(url, error, session);
    }
    return OpenDataSource(url, error);
  };
#elif defined(_WIN32)
  std::shared_ptr<WinHttpSession> session = CreateWinHttpSession();
  return [session](const std::string& url, std::string* error) {
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
      return WinHttpDataSource::Open(url, error, session);
    }
    return OpenDataSource(url, error);
  };
#else
  return OpenDataSource;
#endif
}

std::unique_ptr<DataSource> OpenDataSource(const std::string& url,
                                           std::string* error) {
  std::string path = url;
  size_t scheme_end = url.find("://");
  if (scheme_end != std::string::npos) {
    std::string scheme = url.substr(0, scheme_end);
    if (scheme == "http" || scheme == "https") {
#if defined(__APPLE__)
      return HttpDataSource::Open(url, error);
#elif defined(_WIN32)
      return WinHttpDataSource::Open(url, error);
#elif defined(__ANDROID__)
      return OpenHttpDataSource(url, error);
#elif defined(LYCHEE_WITH_FFMPEG)
      return OpenFFmpegHttpDataSource(url, error);
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
#if defined(_WIN32)
    // file:///C:/dir/file
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') {
      path.erase(0, 1);
    }
#endif
  }
  std::FILE* file = OpenFile(path);
  if (!file) {
    *error = path + ": " + ErrorString(errno);
    return nullptr;
  }
  Seek(file, 0, SEEK_END);
  int64_t size = Tell(file);
  return std::make_unique<FileDataSource>(file, size);
}

}  // namespace lychee
