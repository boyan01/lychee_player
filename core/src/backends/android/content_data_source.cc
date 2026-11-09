#define LOG_TAG "content"

#include "backends/android/content_data_source.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#include "backends/android/jvm.h"
#include "base/log.h"

namespace lychee {

namespace {

// The application Context LycheePlayer.setContext() gave, a global
// reference kept for the process's life.
std::atomic<jobject> application_context{nullptr};

// A pipe's bytes kept behind the reader, for demuxers that step back a
// little (a probe, then the demuxer from the start), as the http source
// keeps them.
constexpr size_t kKeepBehind = 1 << 20;
constexpr size_t kChunk = 64 << 10;

// The file descriptor ContentResolver opens for |url|, owned by the
// caller, and the size the provider reports (-1 if unknown). -1 with
// |error| set if it cannot be opened.
int OpenDescriptor(const std::string& url, int64_t* size, std::string* error) {
  JNIEnv* env = AttachedEnv();
  if (!env) {
    *error = "content:// needs a Java VM";
    return -1;
  }
  jobject context = application_context.load();
  if (!context) {
    *error =
        "content:// needs a Context: call LycheePlayer.setContext() first "
        "(LycheePlayerView and the Flutter plugin do)";
    return -1;
  }
  LocalFrame frame(env);
  jclass uri_class = env->FindClass("android/net/Uri");
  jclass context_class = env->FindClass("android/content/Context");
  jclass resolver_class = env->FindClass("android/content/ContentResolver");
  jclass pfd_class = env->FindClass("android/os/ParcelFileDescriptor");
  if (!uri_class || !context_class || !resolver_class || !pfd_class) {
    *error = TakeException(env);
    return -1;
  }
  jmethodID parse = env->GetStaticMethodID(
      uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
  jmethodID get_resolver =
      env->GetMethodID(context_class, "getContentResolver",
                       "()Landroid/content/ContentResolver;");
  jmethodID open = env->GetMethodID(
      resolver_class, "openFileDescriptor",
      "(Landroid/net/Uri;Ljava/lang/String;)Landroid/os/ParcelFileDescriptor;");
  jmethodID stat_size = env->GetMethodID(pfd_class, "getStatSize", "()J");
  jmethodID detach = env->GetMethodID(pfd_class, "detachFd", "()I");
  if (env->ExceptionCheck()) {
    *error = TakeException(env);
    return -1;
  }
  jobject uri = env->CallStaticObjectMethod(uri_class, parse,
                                            env->NewStringUTF(url.c_str()));
  jobject resolver =
      uri ? env->CallObjectMethod(context, get_resolver) : nullptr;
  // FileNotFoundException, SecurityException (no permission granted for
  // the URI), IllegalArgumentException (no such provider).
  jobject pfd = resolver ? env->CallObjectMethod(resolver, open, uri,
                                                 env->NewStringUTF("r"))
                         : nullptr;
  if (env->ExceptionCheck() || !pfd) {
    std::string why = TakeException(env);
    *error = url + ": " + (why.empty() ? "cannot open" : why);
    return -1;
  }
  *size = env->CallLongMethod(pfd, stat_size);
  const int fd = env->CallIntMethod(pfd, detach);
  if (env->ExceptionCheck() || fd < 0) {
    *error = url + ": " + TakeException(env);
    return -1;
  }
  return fd;
}

class ContentDataSource : public DataSource {
 public:
  ContentDataSource(std::string url, int fd, int64_t size, bool stream)
      : url_(std::move(url)),
        fd_(fd),
        size_(size),
        stream_(stream),
        wake_(stream ? eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK) : -1) {}

  ~ContentDataSource() override {
    if (fd_ >= 0) {
      close(fd_);
    }
    if (wake_ >= 0) {
      close(wake_);
    }
  }

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    if (offset < 0) {
      SetError("read before the start");
      return -1;
    }
    if (interrupted_) {
      return -1;
    }
    if (!stream_) {
      const ssize_t n = pread(fd_, data, size, offset);
      if (n < 0) {
        SetError(strerror(errno));
        return -1;
      }
      return n;
    }
    return ReadStream(offset, data, size);
  }

  int64_t size() const override { return size_; }
  bool seekable() const override { return !stream_; }
  bool reads_ahead() const override { return !stream_; }

  void Interrupt() override {
    interrupted_ = true;
    if (wake_ >= 0) {
      const uint64_t one = 1;
      (void)!write(wake_, &one, sizeof(one));
    }
  }
  void ClearInterrupt() override {
    interrupted_ = false;
    if (wake_ >= 0) {
      uint64_t count;
      (void)!read(wake_, &count, sizeof(count));
    }
  }
  bool interrupted() const override { return interrupted_; }

  std::string error() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  void SetError(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(error);
  }

  // The end of the bytes |kept_| holds, where the pipe is.
  int64_t end() const {
    return kept_start_ + static_cast<int64_t>(kept_.size());
  }

  int64_t ReadStream(int64_t offset, uint8_t* data, size_t size) {
    if (offset < kept_start_) {
      // Behind what is kept: from the start again.
      LOGI("reopening to read at %lld", static_cast<long long>(offset));
      std::string error;
      int64_t ignored = -1;
      const int fd = OpenDescriptor(url_, &ignored, &error);
      if (fd < 0) {
        SetError(error);
        return -1;
      }
      close(fd_);
      fd_ = fd;
      kept_.clear();
      kept_start_ = 0;
      at_end_ = false;
    }
    while (offset >= end() && !at_end_) {
      const int64_t n = Fill();
      if (n < 0) {
        return -1;
      }
    }
    if (offset >= end()) {
      return 0;
    }
    const size_t start = static_cast<size_t>(offset - kept_start_);
    size = std::min(size, kept_.size() - start);
    memcpy(data, kept_.data() + start, size);
    return static_cast<int64_t>(size);
  }

  // Appends the pipe's next bytes to |kept_|: their count, 0 at its end
  // (|at_end_|), or -1 on errors and interrupts.
  int64_t Fill() {
    pollfd fds[2] = {{fd_, POLLIN, 0}, {wake_, POLLIN, 0}};
    for (;;) {
      if (interrupted_) {
        return -1;
      }
      const int ready = poll(fds, 2, -1);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      if (ready < 0) {
        SetError(strerror(errno));
        return -1;
      }
      if (fds[1].revents) {
        return -1;  // Interrupt().
      }
      break;
    }
    const size_t old = kept_.size();
    kept_.resize(old + kChunk);
    ssize_t n;
    do {
      n = read(fd_, kept_.data() + old, kChunk);
    } while (n < 0 && errno == EINTR);
    kept_.resize(old + static_cast<size_t>(std::max<ssize_t>(n, 0)));
    if (n < 0) {
      SetError(strerror(errno));
      return -1;
    }
    if (n == 0) {
      at_end_ = true;
    }
    // Drops what lies far enough behind, a chunk's worth at a time.
    if (kept_.size() > kKeepBehind + kChunk) {
      const size_t drop = kept_.size() - kKeepBehind;
      kept_.erase(kept_.begin(), kept_.begin() + static_cast<ptrdiff_t>(drop));
      kept_start_ += static_cast<int64_t>(drop);
    }
    return n;
  }

  const std::string url_;
  int fd_;
  const int64_t size_;
  const bool stream_;
  // An eventfd that wakes a poll() on the pipe for Interrupt().
  const int wake_;
  std::atomic<bool> interrupted_{false};
  mutable std::mutex mutex_;
  std::string error_;
  // A pipe's bytes from |kept_start_| up to where it is.
  std::vector<uint8_t> kept_;
  int64_t kept_start_ = 0;
  bool at_end_ = false;
};

}  // namespace

std::unique_ptr<DataSource> OpenContentDataSource(const std::string& url,
                                                  std::string* error) {
  int64_t size = -1;
  const int fd = OpenDescriptor(url, &size, error);
  if (fd < 0) {
    return nullptr;
  }
  struct stat info = {};
  const bool regular = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
  if (regular) {
    size = info.st_size;
  } else {
    LOGI("%s is a stream: reading it from the start", url.c_str());
  }
  return std::make_unique<ContentDataSource>(url, fd, size, !regular);
}

}  // namespace lychee

// LycheePlayer.setContext(): keeps the application Context for content://.
extern "C" JNIEXPORT void JNICALL
Java_tech_soit_lychee_LycheePlayer_nativeSetContext(JNIEnv* env,
                                                    jclass,
                                                    jobject context) {
  if (!context || lychee::application_context.load()) {
    return;
  }
  jclass context_class = env->FindClass("android/content/Context");
  jmethodID get_application = env->GetMethodID(
      context_class, "getApplicationContext", "()Landroid/content/Context;");
  jobject application = env->CallObjectMethod(context, get_application);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    application = nullptr;
  }
  // A Context made before the Application (a ContentProvider's in
  // onCreate) may still say null; it serves as well.
  jobject global = env->NewGlobalRef(application ? application : context);
  jobject expected = nullptr;
  if (!lychee::application_context.compare_exchange_strong(expected, global)) {
    env->DeleteGlobalRef(global);
  }
}
