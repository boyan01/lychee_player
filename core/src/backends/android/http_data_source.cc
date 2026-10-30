#define LOG_TAG "http"

#include "backends/android/http_data_source.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

#include "backends/android/jvm.h"
#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

// Bytes kept behind the reader, for demuxers that step back a little.
constexpr size_t kKeepBehind = 1 << 20;
// A read this far ahead of the data reads on instead of starting a new
// request.
constexpr int64_t kSkipAhead = 512 << 10;
// Requests in a row that may fail before reads do, waiting 0, 1 and 3 s
// before each: FFmpeg's http reconnect with reconnect_delay_max=4, as on
// Apple.
constexpr int kMaxRetries = 3;
// Connecting or a connection silent this long fails (FFmpeg's
// rw_timeout).
constexpr jint kTimeoutMs = 10000;
// Bytes asked of the stream per read.
constexpr jint kChunk = 64 << 10;
// Redirects from http to https followed per request, beyond those
// HttpURLConnection follows itself (FFmpeg's http follows five in all).
constexpr int kMaxRedirects = 5;

// Classes and methods, looked up once. All are the boot class path's, which
// FindClass finds from native threads too.
struct Jni {
  jclass url;
  jmethodID url_init;
  jmethodID url_init_relative;
  jmethodID url_open_connection;
  jmethodID url_to_string;
  jmethodID set_connect_timeout;
  jmethodID set_read_timeout;
  jmethodID set_request_property;
  jmethodID get_header_field;
  jmethodID get_content_length;
  jmethodID get_url;
  jmethodID get_input_stream;
  jmethodID get_response_code;
  jmethodID get_response_message;
  jmethodID disconnect;
  jmethodID read;
  jmethodID close;
  // NetworkSecurityPolicy: whether the app's network security config lets
  // cleartext http reach a host.
  jclass policy;
  jmethodID policy_get_instance;
  jmethodID cleartext_permitted;
};

const Jni* GetJni(JNIEnv* env) {
  static Jni jni;
  static bool ok = false;
  static std::once_flag once;
  std::call_once(once, [env] {
    LocalFrame frame(env);
    jclass url = env->FindClass("java/net/URL");
    jclass connection = env->FindClass("java/net/URLConnection");
    jclass http = env->FindClass("java/net/HttpURLConnection");
    jclass stream = env->FindClass("java/io/InputStream");
    jclass policy = env->FindClass("android/security/NetworkSecurityPolicy");
    if (!url || !connection || !http || !stream || !policy) {
      LOGE("%s", TakeException(env).c_str());
      return;
    }
    jni.url = static_cast<jclass>(env->NewGlobalRef(url));
    jni.policy = static_cast<jclass>(env->NewGlobalRef(policy));
    jni.policy_get_instance = env->GetStaticMethodID(
        policy, "getInstance", "()Landroid/security/NetworkSecurityPolicy;");
    jni.cleartext_permitted = env->GetMethodID(
        policy, "isCleartextTrafficPermitted", "(Ljava/lang/String;)Z");
    jni.url_init = env->GetMethodID(url, "<init>", "(Ljava/lang/String;)V");
    jni.url_init_relative =
        env->GetMethodID(url, "<init>", "(Ljava/net/URL;Ljava/lang/String;)V");
    jni.url_open_connection =
        env->GetMethodID(url, "openConnection", "()Ljava/net/URLConnection;");
    jni.url_to_string =
        env->GetMethodID(url, "toString", "()Ljava/lang/String;");
    jni.set_connect_timeout =
        env->GetMethodID(connection, "setConnectTimeout", "(I)V");
    jni.set_read_timeout =
        env->GetMethodID(connection, "setReadTimeout", "(I)V");
    jni.set_request_property =
        env->GetMethodID(connection, "setRequestProperty",
                         "(Ljava/lang/String;Ljava/lang/String;)V");
    jni.get_header_field = env->GetMethodID(
        connection, "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;");
    jni.get_content_length =
        env->GetMethodID(connection, "getContentLengthLong", "()J");
    jni.get_url = env->GetMethodID(connection, "getURL", "()Ljava/net/URL;");
    jni.get_input_stream = env->GetMethodID(connection, "getInputStream",
                                            "()Ljava/io/InputStream;");
    jni.get_response_code = env->GetMethodID(http, "getResponseCode", "()I");
    jni.get_response_message =
        env->GetMethodID(http, "getResponseMessage", "()Ljava/lang/String;");
    jni.disconnect = env->GetMethodID(http, "disconnect", "()V");
    jni.read = env->GetMethodID(stream, "read", "([BII)I");
    jni.close = env->GetMethodID(stream, "close", "()V");
    if (env->ExceptionCheck()) {
      LOGE("%s", TakeException(env).c_str());
      return;
    }
    ok = true;
  });
  return ok ? &jni : nullptr;
}

std::string ToString(JNIEnv* env, jstring string) {
  if (!string) {
    return "";
  }
  const char* chars = env->GetStringUTFChars(string, nullptr);
  std::string result = chars ? chars : "";
  env->ReleaseStringUTFChars(string, chars);
  return result;
}

std::string HeaderField(JNIEnv* env,
                        const Jni& jni,
                        jobject connection,
                        const char* name) {
  jstring value = static_cast<jstring>(env->CallObjectMethod(
      connection, jni.get_header_field, env->NewStringUTF(name)));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return "";
  }
  return ToString(env, value);
}

class HttpDataSource : public DataSource {
 public:
  HttpDataSource(const Jni& jni, std::string url)
      : jni_(jni), url_(std::move(url)) {}

  ~HttpDataSource() override {
    if (JNIEnv* env = AttachedEnv()) {
      Disconnect(env);
      if (chunk_) {
        env->DeleteGlobalRef(chunk_);
      }
    }
  }

  int64_t Read(int64_t offset, uint8_t* data, size_t size) override {
    JNIEnv* env = AttachedEnv();
    if (!env) {
      SetError("no Java VM");
      return -1;
    }
    for (;;) {
      int64_t total;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (interrupted_) {
          return -1;
        }
        total = size_;
      }
      if (total >= 0 && offset >= total) {
        return 0;
      }
      if (offset >= offset_ && offset < end()) {
        return CopyOut(offset, data, size);
      }
      if (!stream_ || offset < offset_ || offset > end() + kSkipAhead) {
        if (stream_) {
          LOGI("range request from %lld", static_cast<long long>(offset));
        }
        if (!Connect(env, offset) && !WaitToRetry(offset)) {
          return -1;
        }
        continue;
      }
      int64_t n = ReadStream(env);
      if (n > 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        retries_ = 0;
        continue;
      }
      Disconnect(env);
      if (n == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ < 0 || end() >= size_) {
          return 0;
        }
        error_ = "connection closed before the end";
      }
      // The connection broke or closed early: ask again from here.
      if (!WaitToRetry(offset)) {
        return -1;
      }
    }
  }

  int64_t size() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
  }

  bool seekable() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return ranges_;
  }

  std::string url() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return location_;
  }

  bool cleartext_refused() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return cleartext_refused_;
  }

  // Disconnecting fails the connect or read in flight on the reader's
  // thread at once, where a flag alone would wait out its timeout.
  void Interrupt() override {
    std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = true;
    condition_.notify_all();
    if (connection_) {
      if (JNIEnv* env = AttachedEnv()) {
        env->CallVoidMethod(connection_, jni_.disconnect);
        env->ExceptionClear();
      }
    }
  }

  void ClearInterrupt() override {
    std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = false;
    retries_ = 0;
  }

  bool interrupted() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupted_;
  }

  std::string error() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupted_ ? "interrupted" : error_;
  }

  int64_t transfer_rate() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (receive_micros_ <= 0 || received_ <= 0) {
      return -1;
    }
    const long double rate = static_cast<long double>(received_) * 8 *
                             kMicrosPerSecond /
                             static_cast<long double>(receive_micros_);
    return rate >= static_cast<long double>(std::numeric_limits<int64_t>::max())
               ? std::numeric_limits<int64_t>::max()
               : static_cast<int64_t>(rate);
  }

 private:
  int64_t end() const {
    return offset_ + static_cast<int64_t>(data_.size() - head_);
  }

  void SetError(std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(error);
  }

  int64_t CopyOut(int64_t offset, uint8_t* data, size_t size) {
    size_t start = head_ + static_cast<size_t>(offset - offset_);
    size_t n = std::min(size, data_.size() - start);
    memcpy(data, data_.data() + start, n);
    // Drop what is well behind the reader.
    size_t keep_from = start + n > kKeepBehind ? start + n - kKeepBehind : 0;
    if (keep_from > head_) {
      offset_ += static_cast<int64_t>(keep_from - head_);
      head_ = keep_from;
      if (head_ > data_.size() / 2) {
        data_.erase(data_.begin(),
                    data_.begin() + static_cast<ptrdiff_t>(head_));
        head_ = 0;
      }
    }
    return static_cast<int64_t>(n);
  }

  // Whether to try again, after waiting; false once retries are used up,
  // the server refused, or on an interrupt.
  bool WaitToRetry(int64_t offset) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (interrupted_) {
      return false;
    }
    if (refused_ || retries_ >= kMaxRetries) {
      if (error_.empty()) {
        error_ = "connection failed";
      }
      return false;
    }
    int delay = (1 << retries_) - 1;
    retries_++;
    LOGW("reconnecting at %lld in %d s after: %s",
         static_cast<long long>(offset), delay, error_.c_str());
    return !condition_.wait_for(lock, std::chrono::seconds(delay),
                                [this] { return interrupted_; });
  }

  // Replaces the request with one from |from|. False with error_ set if it
  // gave no stream.
  bool Connect(JNIEnv* env, int64_t from) {
    Disconnect(env);
    LocalFrame frame(env);
    offset_ = from;
    data_.clear();
    head_ = 0;
    discard_ = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      error_.clear();
      refused_ = false;
    }
    if (!chunk_) {
      chunk_ =
          static_cast<jbyteArray>(env->NewGlobalRef(env->NewByteArray(kChunk)));
    }
    // HttpURLConnection follows redirects within a scheme only. Those from
    // http to https are followed here, as NSURLSession and FFmpeg follow
    // them; never back to http.
    std::string target = url_;
    jobject connection = nullptr;
    jint status = 0;
    int64_t began = 0;
    for (int redirects = 0;; ++redirects) {
      jobject url = env->NewObject(jni_.url, jni_.url_init,
                                   env->NewStringUTF(target.c_str()));
      if (!url) {
        return Fail(env, true);
      }
      if (CleartextRefused(env, url)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "cleartext http refused by the network security config";
        cleartext_refused_ = true;
        refused_ = true;
        return false;
      }
      connection = env->CallObjectMethod(url, jni_.url_open_connection);
      if (!connection) {
        return Fail(env, true);
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (interrupted_) {
          return false;
        }
        connection_ = env->NewGlobalRef(connection);
      }
      env->CallVoidMethod(connection, jni_.set_connect_timeout, kTimeoutMs);
      env->CallVoidMethod(connection, jni_.set_read_timeout, kTimeoutMs);
      // From 0 too, as FFmpeg does: a 206 says the server takes ranges.
      std::string range = "bytes=" + std::to_string(from) + "-";
      env->CallVoidMethod(connection, jni_.set_request_property,
                          env->NewStringUTF("Range"),
                          env->NewStringUTF(range.c_str()));
      // Offsets are into the resource as stored: no transparent gzip.
      env->CallVoidMethod(connection, jni_.set_request_property,
                          env->NewStringUTF("Accept-Encoding"),
                          env->NewStringUTF("identity"));
      began = MonotonicMicros();
      status = env->CallIntMethod(connection, jni_.get_response_code);
      if (env->ExceptionCheck()) {
        return Fail(env, false);
      }
      if (status < 300 || status >= 400 || redirects == kMaxRedirects) {
        break;
      }
      std::string location = HeaderField(env, jni_, connection, "Location");
      jobject next = location.empty()
                         ? nullptr
                         : env->NewObject(jni_.url, jni_.url_init_relative, url,
                                          env->NewStringUTF(location.c_str()));
      std::string next_url;
      if (next) {
        next_url = ToString(env, static_cast<jstring>(env->CallObjectMethod(
                                     next, jni_.url_to_string)));
      }
      env->ExceptionClear();
      if (next_url.rfind("https://", 0) != 0) {
        break;
      }
      LOGI("following a redirect from http to https");
      Disconnect(env);
      target = std::move(next_url);
    }
    if (status >= 300) {
      std::string message =
          ToString(env, static_cast<jstring>(env->CallObjectMethod(
                            connection, jni_.get_response_message)));
      env->ExceptionClear();
      std::lock_guard<std::mutex> lock(mutex_);
      error_ = "HTTP " + std::to_string(status) + " " + message;
      if (status < 400) {
        // A redirect to http, or one too many.
        error_ += " to " + HeaderField(env, jni_, connection, "Location");
      }
      // Server errors may pass; FFmpeg's reconnect_on_http_error=5xx.
      refused_ = status < 500;
      return false;
    }
    std::string location;
    if (jobject final_url = env->CallObjectMethod(connection, jni_.get_url)) {
      location = ToString(env, static_cast<jstring>(env->CallObjectMethod(
                                   final_url, jni_.url_to_string)));
    }
    env->ExceptionClear();
    std::string accept = HeaderField(env, jni_, connection, "Accept-Ranges");
    int64_t total = -1;
    if (status == 206) {
      // Content-Range: bytes first-last/total
      std::string content_range =
          HeaderField(env, jni_, connection, "Content-Range");
      size_t slash = content_range.find('/');
      if (slash != std::string::npos) {
        const char* end = content_range.data() + content_range.size();
        const auto parsed =
            std::from_chars(content_range.data() + slash + 1, end, total);
        if (parsed.ec != std::errc() || parsed.ptr != end || total <= 0) {
          total = -1;
        }
      }
    } else {
      // The whole resource, whatever was asked for.
      discard_ = from;
      total = env->CallLongMethod(connection, jni_.get_content_length);
      env->ExceptionClear();
    }
    jobject stream = env->CallObjectMethod(connection, jni_.get_input_stream);
    if (!stream) {
      return Fail(env, false);
    }
    stream_ = env->NewGlobalRef(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    if (location != url_) {
      location_ = location;
    }
    receive_micros_ += MonotonicMicros() - began;
    ranges_ = status == 206 || accept.find("bytes") != std::string::npos;
    if (total >= 0) {
      size_ = total;
    }
    return true;
  }

  // Takes the pending exception into error_. |refused| for exceptions that
  // another try would meet again.
  bool Fail(JNIEnv* env, bool refused) {
    std::string error = TakeException(env);
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_.empty()) {
      error_ = error.empty() ? "network error" : error;
    }
    refused_ = refused;
    return false;
  }

  // Whether the app's network security config refuses cleartext http to
  // |url|'s host (by default from API 28), which HttpURLConnection would
  // fail with a plain IOException.
  bool CleartextRefused(JNIEnv* env, jobject url) {
    jclass url_class = env->GetObjectClass(url);
    jmethodID get_protocol =
        env->GetMethodID(url_class, "getProtocol", "()Ljava/lang/String;");
    jmethodID get_host =
        env->GetMethodID(url_class, "getHost", "()Ljava/lang/String;");
    std::string protocol = ToString(
        env, static_cast<jstring>(env->CallObjectMethod(url, get_protocol)));
    if (protocol != "http") {
      env->ExceptionClear();
      return false;
    }
    jobject policy =
        env->CallStaticObjectMethod(jni_.policy, jni_.policy_get_instance);
    jobject host = env->CallObjectMethod(url, get_host);
    bool permitted =
        policy && host &&
        env->CallBooleanMethod(policy, jni_.cleartext_permitted, host);
    env->ExceptionClear();
    return !permitted;
  }

  // Appends what the stream gives. Returns the bytes read, 0 at its end,
  // or -1 with error_ set.
  int64_t ReadStream(JNIEnv* env) {
    const int64_t began = MonotonicMicros();
    jint n = env->CallIntMethod(stream_, jni_.read, chunk_, 0, kChunk);
    if (env->ExceptionCheck()) {
      Fail(env, false);
      return -1;
    }
    if (n < 0) {
      return 0;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      received_ += n;
      receive_micros_ += MonotonicMicros() - began;
    }
    size_t skip = static_cast<size_t>(std::min<int64_t>(discard_, n));
    discard_ -= static_cast<int64_t>(skip);
    size_t old_size = data_.size();
    data_.resize(old_size + static_cast<size_t>(n) - skip);
    env->GetByteArrayRegion(chunk_, static_cast<jsize>(skip),
                            n - static_cast<jint>(skip),
                            reinterpret_cast<jbyte*>(data_.data() + old_size));
    return n;
  }

  void Disconnect(JNIEnv* env) {
    jobject connection;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connection = connection_;
      connection_ = nullptr;
    }
    // Disconnecting first, so closing does not drain the rest of the
    // response for reuse.
    if (connection) {
      env->CallVoidMethod(connection, jni_.disconnect);
      env->ExceptionClear();
      env->DeleteGlobalRef(connection);
    }
    if (stream_) {
      env->CallVoidMethod(stream_, jni_.close);
      env->ExceptionClear();
      env->DeleteGlobalRef(stream_);
      stream_ = nullptr;
    }
  }

  const Jni& jni_;
  const std::string url_;

  // The reader's thread only.
  jobject stream_ = nullptr;
  jbyteArray chunk_ = nullptr;
  // Bytes [offset_, end()) of the resource, from data_[head_].
  int64_t offset_ = 0;
  std::vector<uint8_t> data_;
  size_t head_ = 0;
  // Bytes still to drop from the response: the server ignored the range.
  int64_t discard_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  // The request in flight, for Interrupt().
  jobject connection_ = nullptr;
  int64_t size_ = -1;
  // Only connection/stream I/O is timed; time spent consuming buffered
  // packets belongs to the caller rather than the link.
  int64_t received_ = 0;
  int64_t receive_micros_ = 0;
  bool ranges_ = false;
  std::string location_;
  bool cleartext_refused_ = false;
  // A 4xx status or a malformed URL: retrying will not help.
  bool refused_ = false;
  // Requests since data last arrived.
  int retries_ = 0;
  bool interrupted_ = false;
  std::string error_;
};

}  // namespace

bool HttpDataSourceAvailable() {
  return GetJavaVm() != nullptr;
}

std::unique_ptr<DataSource> OpenHttpDataSource(const std::string& url,
                                               std::string* error) {
  JNIEnv* env = AttachedEnv();
  const Jni* jni = env ? GetJni(env) : nullptr;
  if (!jni) {
    *error = "no HTTP client: no Java VM";
    return nullptr;
  }
  // Like NSURLSession on Apple, nothing is requested before the first
  // read.
  return std::make_unique<HttpDataSource>(*jni, url);
}

}  // namespace lychee
