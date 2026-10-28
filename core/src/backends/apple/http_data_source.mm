#define LOG_TAG "http"

#include "backends/apple/http_data_source.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "base/log.h"

namespace lychee {

namespace {

// The request pauses with this much buffered ahead of the reader, and
// resumes below half of it.
constexpr size_t kMaxAhead = 16 << 20;
// Bytes kept behind the reader, for demuxers that step back a little.
constexpr size_t kKeepBehind = 1 << 20;
// A read this far ahead of the data waits for it instead of starting a
// new request.
constexpr int64_t kSkipAhead = 512 << 10;
// Requests in a row that may fail (dropped connections, server errors)
// before reads do, waiting 0, 1 and 3 s before each: FFmpeg's http
// reconnect with reconnect_delay_max=4, so both builds ride out the same
// outages.
constexpr int kMaxRetries = 3;
// A connection silent this long counts as dropped (FFmpeg's rw_timeout).
constexpr NSTimeInterval kIdleTimeout = 10;

}  // namespace

struct HttpState;

}  // namespace lychee

// Hands each task's events to the HttpState that started it.
@interface LycheeHttpDelegate : NSObject <NSURLSessionDataDelegate>
- (void)addTask:(NSURLSessionTask*)task
          state:(std::weak_ptr<lychee::HttpState>)state;
@end

namespace lychee {

class HttpSession {
 public:
  HttpSession() {
    NSOperationQueue* queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    delegate_ = [[LycheeHttpDelegate alloc] init];
    session_ = [NSURLSession
        sessionWithConfiguration:[NSURLSessionConfiguration
                                     ephemeralSessionConfiguration]
                        delegate:delegate_
                   delegateQueue:queue];
  }
  // The session holds its delegate until invalidated.
  ~HttpSession() { [session_ invalidateAndCancel]; }
  HttpSession(const HttpSession&) = delete;
  HttpSession& operator=(const HttpSession&) = delete;

  NSURLSessionDataTask* Start(NSURLRequest* request,
                              std::weak_ptr<HttpState> state) {
    NSURLSessionDataTask* task = [session_ dataTaskWithRequest:request];
    [delegate_ addTask:task state:std::move(state)];
    return task;
  }

 private:
  NSURLSession* session_ = nil;
  LycheeHttpDelegate* delegate_ = nil;
};

std::shared_ptr<HttpSession> CreateHttpSession() {
  @autoreleasepool {
    return std::make_shared<HttpSession>();
  }
}

struct HttpState : std::enable_shared_from_this<HttpState> {
  std::mutex mutex;
  std::condition_variable condition;
  NSURL* url = nil;
  std::shared_ptr<HttpSession> session;
  NSURLSessionDataTask* task = nil;
  // Bytes [offset, offset + data.size() - head) of the resource, from
  // data[head].
  int64_t offset = 0;
  std::vector<uint8_t> data;
  size_t head = 0;
  // Bytes still to drop from the response: the server ignored the range.
  int64_t discard = 0;
  int64_t size = -1;
  bool complete = false;
  // A 4xx status: retrying will not help.
  bool refused = false;
  // The last response took a range request (206, or Accept-Ranges).
  bool ranges = false;
  // Where the last response came from, after redirects.
  std::string location;
  // App Transport Security refused cleartext http.
  bool cleartext_refused = false;
  // Requests since data last arrived.
  int retries = 0;
  bool suspended = false;
  bool interrupted = false;
  std::string error;

  int64_t end() const { return offset + static_cast<int64_t>(data.size() - head); }

  // Replaces the request with one from |from|. Under |mutex|.
  void Restart(int64_t from) {
    [task cancel];
    offset = from;
    data.clear();
    head = 0;
    discard = 0;
    complete = false;
    refused = false;
    suspended = false;
    error.clear();
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
    request.timeoutInterval = kIdleTimeout;
    // From 0 too, as FFmpeg does: a 206 says the server takes ranges.
    [request setValue:[NSString stringWithFormat:@"bytes=%lld-", from]
        forHTTPHeaderField:@"Range"];
    task = session->Start(request, weak_from_this());
    [task resume];
  }
};

}  // namespace lychee

@implementation LycheeHttpDelegate {
  std::mutex _mutex;
  std::unordered_map<NSUInteger, std::weak_ptr<lychee::HttpState>> _states;
}

- (void)addTask:(NSURLSessionTask*)task
          state:(std::weak_ptr<lychee::HttpState>)state {
  std::lock_guard<std::mutex> lock(_mutex);
  _states[task.taskIdentifier] = std::move(state);
}

- (std::shared_ptr<lychee::HttpState>)stateOf:(NSURLSessionTask*)task {
  std::lock_guard<std::mutex> lock(_mutex);
  auto it = _states.find(task.taskIdentifier);
  return it != _states.end() ? it->second.lock() : nullptr;
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))handler {
  std::shared_ptr<lychee::HttpState> state = [self stateOf:task];
  if (!state) {
    handler(NSURLSessionResponseCancel);
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (task != state->task) {
    handler(NSURLSessionResponseCancel);
    return;
  }
  auto* http = static_cast<NSHTTPURLResponse*>(response);
  NSInteger status = http.statusCode;
  if (status >= 400) {
    state->error =
        "HTTP " + std::to_string(status) + " " +
        [NSHTTPURLResponse localizedStringForStatusCode:status].UTF8String;
    // Server errors may pass; FFmpeg's reconnect_on_http_error=5xx.
    state->refused = status < 500;
    state->complete = true;
    state->condition.notify_all();
    handler(NSURLSessionResponseCancel);
    return;
  }
  long long length = response.expectedContentLength;
  if (![http.URL isEqual:state->url]) {
    state->location = http.URL.absoluteString.UTF8String ?: "";
  }
  NSString* accept = http.allHeaderFields[@"Accept-Ranges"];
  state->ranges =
      status == 206 ||
      (accept && [accept rangeOfString:@"bytes"].location != NSNotFound);
  if (status == 206) {
    // Content-Range: bytes first-last/total
    NSString* range = http.allHeaderFields[@"Content-Range"];
    NSRange slash = [range rangeOfString:@"/"];
    if (slash.location != NSNotFound) {
      long long total = [[range substringFromIndex:slash.location + 1] longLongValue];
      if (total > 0) {
        state->size = total;
      }
    }
  } else {
    // The whole resource, whatever was asked for.
    state->discard = state->offset;
    if (length >= 0) {
      state->size = length;
    }
  }
  handler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data {
  std::shared_ptr<lychee::HttpState> state = [self stateOf:task];
  if (!state) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (task != state->task) {
    return;
  }
  [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range, BOOL*) {
    auto* begin = static_cast<const uint8_t*>(bytes);
    size_t length = range.length;
    size_t skip = static_cast<size_t>(
        std::min<int64_t>(state->discard, static_cast<int64_t>(length)));
    state->discard -= static_cast<int64_t>(skip);
    state->data.insert(state->data.end(), begin + skip, begin + length);
  }];
  state->retries = 0;
  if (state->data.size() - state->head > lychee::kMaxAhead && !state->suspended) {
    state->suspended = true;
    [task suspend];
  }
  state->condition.notify_all();
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error {
  std::shared_ptr<lychee::HttpState> state = [self stateOf:task];
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _states.erase(task.taskIdentifier);
  }
  if (!state) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  if (task != state->task) {
    return;
  }
  state->complete = true;
  if (error && state->error.empty()) {
    state->error = error.localizedDescription.UTF8String ?: "network error";
  }
  if (error.code == NSURLErrorAppTransportSecurityRequiresSecureConnection) {
    state->cleartext_refused = true;
    state->refused = true;
  }
  state->condition.notify_all();
}

@end

namespace lychee {

std::unique_ptr<DataSource> HttpDataSource::Open(
    const std::string& url,
    std::string* error,
    std::shared_ptr<HttpSession> session) {
  @autoreleasepool {
    NSURL* nsurl = [NSURL URLWithString:@(url.c_str())];
    if (!nsurl) {
      *error = "malformed URL";
      return nullptr;
    }
    auto state = std::make_shared<HttpState>();
    state->url = nsurl;
    state->session = session ? std::move(session) : CreateHttpSession();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->Restart(0);
    return std::unique_ptr<DataSource>(new HttpDataSource(state));
  }
}

HttpDataSource::HttpDataSource(std::shared_ptr<HttpState> state)
    : state_(std::move(state)) {}

HttpDataSource::~HttpDataSource() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  [state_->task cancel];
}

int64_t HttpDataSource::Read(int64_t offset, uint8_t* data, size_t size) {
  @autoreleasepool {
    HttpState& state = *state_;
    std::unique_lock<std::mutex> lock(state.mutex);
    for (;;) {
      if (state.interrupted) {
        return -1;
      }
      if (state.size >= 0 && offset >= state.size) {
        return 0;
      }
      int64_t end = state.end();
      if (offset >= state.offset && offset < end) {
        size_t start = state.head + static_cast<size_t>(offset - state.offset);
        size_t n = std::min(size, state.data.size() - start);
        memcpy(data, state.data.data() + start, n);
        // Drop what is well behind the reader.
        size_t keep_from = start + n > kKeepBehind ? start + n - kKeepBehind : 0;
        if (keep_from > state.head) {
          state.offset += static_cast<int64_t>(keep_from - state.head);
          state.head = keep_from;
          if (state.head > state.data.size() / 2) {
            state.data.erase(state.data.begin(),
                             state.data.begin() + static_cast<ptrdiff_t>(state.head));
            state.head = 0;
          }
        }
        if (state.suspended && state.data.size() - start - n < kMaxAhead / 2) {
          state.suspended = false;
          [state.task resume];
        }
        return static_cast<int64_t>(n);
      }
      if (offset >= state.offset && offset <= end + kSkipAhead) {
        if (!state.complete) {
          if (state.suspended) {
            state.suspended = false;
            [state.task resume];
          }
          state.condition.wait(lock);
          continue;
        }
        bool ended = state.error.empty() &&
                     (state.size < 0 || end >= state.size);
        if (offset >= end && ended) {
          return 0;
        }
        if (offset >= end) {
          // The connection broke (or closed early): ask again from here.
          if (state.refused || state.retries >= kMaxRetries) {
            if (state.error.empty()) {
              state.error = "connection closed before the end";
            }
            return -1;
          }
          int delay = (1 << state.retries) - 1;
          state.retries++;
          LOGW("reconnecting at %lld in %d s after: %s",
               static_cast<long long>(offset), delay,
               state.error.empty() ? "connection closed" : state.error.c_str());
          if (state.condition.wait_for(
                  lock, std::chrono::seconds(delay),
                  [&state] { return state.interrupted; })) {
            return -1;
          }
          state.Restart(offset);
          continue;
        }
      }
      LOGI("range request from %lld", static_cast<long long>(offset));
      state.Restart(offset);
    }
  }
}

bool HttpDataSource::seekable() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->ranges;
}

bool HttpDataSource::cleartext_refused() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->cleartext_refused;
}

std::string HttpDataSource::url() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->location;
}

int64_t HttpDataSource::size() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->size;
}

void HttpDataSource::Interrupt() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->interrupted = true;
  state_->condition.notify_all();
}

void HttpDataSource::ClearInterrupt() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->interrupted = false;
}

bool HttpDataSource::interrupted() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->interrupted;
}

std::string HttpDataSource::error() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->interrupted ? "interrupted" : state_->error;
}

}  // namespace lychee
