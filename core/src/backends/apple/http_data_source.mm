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
#include "base/time.h"
#include "media/error.h"

namespace lychee {

namespace {

// The request pauses with this much buffered ahead of the reader, and
// resumes below half of it.
constexpr size_t kMaxAhead = 16 << 20;
// After the reader jumps, the request asks for this many bytes, then for
// twice as many from their end each time the reader is past half of
// them, and for the rest once that reaches kMaxAhead. Demuxers probing
// (an index at the end, an MPEG-TS seek's bisection) leave after a
// little; an open-ended request has as much more on its way as the link
// carries until the cancel, which pausing it does not stop (TCP buffers
// megabytes), so a seek in a large MPEG-TS file over a fast LAN fetched
// three times the data. 2 MB holds what a probe reads (up to the next
// keyframe), so probes take no extra round trip; 256 KB made seeks
// slower over a high-latency link. The request at the open is
// open-ended: most readers (HLS segments, progressive playback) go on
// from there.
constexpr int64_t kFirstRange = 2 << 20;
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
  HttpSession() = default;
  // The session holds its delegate until invalidated.
  ~HttpSession() { [session_ invalidateAndCancel]; }
  HttpSession(const HttpSession&) = delete;
  HttpSession& operator=(const HttpSession&) = delete;

  NSURLSessionDataTask* Start(NSURLRequest* request,
                              std::weak_ptr<HttpState> state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_) {
      // Made on the first request: a demuxer's opener may open only files.
      NSOperationQueue* queue = [[NSOperationQueue alloc] init];
      queue.maxConcurrentOperationCount = 1;
      delegate_ = [[LycheeHttpDelegate alloc] init];
      session_ = [NSURLSession
          sessionWithConfiguration:[NSURLSessionConfiguration
                                       ephemeralSessionConfiguration]
                          delegate:delegate_
                     delegateQueue:queue];
    }
    NSURLSessionDataTask* task = [session_ dataTaskWithRequest:request];
    [delegate_ addTask:task state:std::move(state)];
    return task;
  }

 private:
  std::mutex mutex_;
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
  // The caller's, sent with every request.
  NSDictionary<NSString*, NSString*>* headers = nil;
  std::shared_ptr<HttpSession> session;
  NSURLSessionDataTask* task = nil;
  // Bytes [offset, offset + data.size() - head) of the resource, from
  // data[head].
  int64_t offset = 0;
  std::vector<uint8_t> data;
  size_t head = 0;
  // Bytes still to drop from the response: the server ignored the range.
  int64_t discard = 0;
  // Where the request started and how many bytes it asked for, 0 for all
  // the rest; and how many the next one from the end asks for.
  int64_t request_start = 0;
  int64_t range = 0;
  int64_t next_range = 0;
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
  // For transfer_rate(): bytes and microseconds of the spans closed so
  // far, and the open one (from a request's start or resume to its last
  // bytes).
  int64_t received = 0;
  int64_t receive_micros = 0;
  int64_t span_start = -1;
  int64_t span_last = -1;
  int64_t span_bytes = 0;

  void EndSpan() {
    if (span_start >= 0 && span_last > span_start) {
      received += span_bytes;
      receive_micros += span_last - span_start;
    }
    span_start = -1;
    span_bytes = 0;
  }
  void BeginSpan() {
    EndSpan();
    span_start = MonotonicMicros();
    span_last = -1;
  }
  void Resume() {
    suspended = false;
    BeginSpan();
    [task resume];
  }

  int64_t end() const { return offset + static_cast<int64_t>(data.size() - head); }

  // Whether the request got all it asked for, short of the end of the
  // resource: the next one continues from there.
  bool RangeDone() const {
    return complete && error.empty() && range > 0 && discard == 0 &&
           end() == request_start + range && (size < 0 || end() < size);
  }

  // Replaces the request with one from |from| that asks for |first|
  // bytes (0: all the rest). Under |mutex|.
  void Restart(int64_t from, int64_t first) {
    [task cancel];
    offset = from;
    data.clear();
    head = 0;
    next_range = first;
    Request(from);
  }

  // Asks for the next range from the end, keeping what is buffered.
  void Continue() { Request(end()); }

  void Request(int64_t from) {
    discard = 0;
    complete = false;
    refused = false;
    suspended = false;
    error.clear();
    request_start = from;
    range = next_range < static_cast<int64_t>(kMaxAhead) ? next_range : 0;
    next_range = range * 2;
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
    request.timeoutInterval = kIdleTimeout;
    [headers enumerateKeysAndObjectsUsingBlock:^(NSString* name,
                                                 NSString* value, BOOL*) {
      [request setValue:value forHTTPHeaderField:name];
    }];
    // From 0 too, as FFmpeg does: a 206 says the server takes ranges.
    NSString* bytes = range > 0
                          ? [NSString stringWithFormat:@"bytes=%lld-%lld", from,
                                                       from + range - 1]
                          : [NSString stringWithFormat:@"bytes=%lld-", from];
    [request setValue:bytes forHTTPHeaderField:@"Range"];
    task = session->Start(request, weak_from_this());
    BeginSpan();
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
    state->error = lychee::TagError(
        lychee::ErrorKind::kHttp,
        "HTTP " + std::to_string(status) + " " +
            [NSHTTPURLResponse localizedStringForStatusCode:status].UTF8String,
        static_cast<int>(status));
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
    state->discard = state->request_start;
    state->range = 0;
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
  state->span_bytes += static_cast<int64_t>(data.length);
  state->span_last = lychee::MonotonicMicros();
  state->retries = 0;
  if (state->data.size() - state->head > lychee::kMaxAhead && !state->suspended) {
    state->suspended = true;
    state->EndSpan();
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
  state->EndSpan();
  if (error && state->error.empty()) {
    // NSURLErrorDomain's errors are the connection's, but for a policy
    // that no retry gets past.
    const bool network =
        [error.domain isEqualToString:NSURLErrorDomain] &&
        error.code != NSURLErrorAppTransportSecurityRequiresSecureConnection;
    state->error = lychee::TagError(
        network ? lychee::ErrorKind::kNetwork : lychee::ErrorKind::kOther,
        error.localizedDescription.UTF8String ?: "network error");
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
    const std::string& headers,
    std::shared_ptr<HttpSession> session) {
  @autoreleasepool {
    NSURL* nsurl = [NSURL URLWithString:@(url.c_str())];
    if (!nsurl) {
      *error = "malformed URL";
      return nullptr;
    }
    auto state = std::make_shared<HttpState>();
    state->url = nsurl;
    if (!headers.empty()) {
      NSMutableDictionary<NSString*, NSString*>* fields =
          [NSMutableDictionary dictionary];
      for (const auto& [name, value] : SplitHttpHeaders(headers)) {
        NSString* field = [NSString stringWithUTF8String:name.c_str()];
        NSString* text = [NSString stringWithUTF8String:value.c_str()];
        if (!field || !text) {
          *error = "header not UTF-8: " + name;
          return nullptr;
        }
        fields[field] = text;
      }
      state->headers = fields;
    }
    state->session = session ? std::move(session) : CreateHttpSession();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->Restart(0, 0);
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
        const size_t left = state.data.size() - start - n;
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
        if (state.suspended && left < kMaxAhead / 2) {
          state.Resume();
        } else if (state.RangeDone() &&
                   left < static_cast<size_t>(state.range / 2)) {
          state.Continue();
        }
        return static_cast<int64_t>(n);
      }
      if (offset >= state.offset && offset <= end + kSkipAhead) {
        if (state.RangeDone()) {
          state.Continue();
          continue;
        }
        if (!state.complete) {
          if (state.suspended) {
            state.Resume();
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
          // Not a stream of unknown size whose server takes no ranges (a
          // live stream): it would send what it has now, and dropping
          // all that came before to get back here takes as long as it
          // played. The demuxer reconnects itself if it can (FLV).
          if (state.refused || state.retries >= kMaxRetries ||
              (offset > 0 && !state.ranges && state.size < 0)) {
            if (state.error.empty()) {
              state.error = TagError(ErrorKind::kNetwork,
                                     "connection closed before the end");
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
          state.Restart(offset, 0);
          continue;
        }
      }
      LOGI("range request from %lld", static_cast<long long>(offset));
      state.Restart(offset, kFirstRange);
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

int64_t HttpDataSource::transfer_rate() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const HttpState& state = *state_;
  int64_t bytes = state.received;
  int64_t micros = state.receive_micros;
  if (state.span_start >= 0 && state.span_last > state.span_start) {
    bytes += state.span_bytes;
    micros += state.span_last - state.span_start;
  }
  return micros > 0 ? bytes * 8 * kMicrosPerSecond / micros : -1;
}

std::string HttpDataSource::error() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->interrupted ? "interrupted" : state_->error;
}

}  // namespace lychee
