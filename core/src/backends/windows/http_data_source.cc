#define LOG_TAG "http"

#include "backends/windows/http_data_source.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include "base/log.h"

namespace lychee {

namespace {

// As Apple's HttpDataSource (http_data_source.mm), which explains them.
constexpr size_t kMaxAhead = 16 << 20;
constexpr size_t kKeepBehind = 1 << 20;
constexpr int64_t kSkipAhead = 512 << 10;
constexpr int kMaxRetries = 3;
// Resolving, connecting, sending and each receive (FFmpeg's rw_timeout).
constexpr int kIdleTimeoutMs = 10000;
// One WinHttpReadData() call's worth.
constexpr DWORD kChunk = 64 * 1024;

std::wstring Widen(const std::string& text) {
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(std::max(size, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), size);
  return wide;
}

std::string Narrow(const std::wstring& wide) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                       static_cast<int>(wide.size()), nullptr,
                                       0, nullptr, nullptr);
  std::string text(static_cast<size_t>(std::max(size, 0)), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      text.data(), size, nullptr, nullptr);
  return text;
}

std::string ErrorString(DWORD error) {
  wchar_t* message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE |
          FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      GetModuleHandleW(L"winhttp.dll"), error, 0,
      reinterpret_cast<wchar_t*>(&message), 0, nullptr);
  std::wstring text(message ? message : L"", length);
  LocalFree(message);
  while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' ||
                           text.back() == L' ' || text.back() == L'.')) {
    text.pop_back();
  }
  return "WinHTTP error " + std::to_string(error) +
         (text.empty() ? "" : ": " + Narrow(text));
}

// A response header, or empty.
std::wstring Header(HINTERNET request, DWORD info) {
  DWORD size = 0;
  WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                      &size, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
    return L"";
  }
  std::wstring value(size / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(request, info, WINHTTP_HEADER_NAME_BY_INDEX,
                           value.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
    return L"";
  }
  value.resize(size / sizeof(wchar_t));
  return value;
}

// Where the response came from, after redirects.
std::wstring FinalUrl(HINTERNET request) {
  DWORD size = 0;
  WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &size);
  if (size == 0) {
    return L"";
  }
  std::wstring url(size / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, url.data(), &size)) {
    return L"";
  }
  url.resize(wcsnlen(url.c_str(), url.size()));
  return url;
}

}  // namespace

struct WinHttpState {
  std::mutex mutex;
  std::condition_variable condition;
  // Set by Open().
  std::wstring url;
  std::wstring path;
  bool secure = false;
  HINTERNET session = nullptr;
  HINTERNET connection = nullptr;
  // The thread's request once it has a response; whoever closes it nulls
  // it. Closing it aborts a WinHttpReadData() blocked on it.
  HINTERNET request = nullptr;
  // Bumped by each Restart(); the thread serves the latest.
  uint64_t generation = 0;
  bool shutdown = false;
  // As HttpState in http_data_source.mm.
  int64_t offset = 0;
  std::vector<uint8_t> data;
  size_t head = 0;
  int64_t discard = 0;
  int64_t size = -1;
  bool complete = false;
  bool refused = false;
  bool ranges = false;
  std::string location;
  int retries = 0;
  bool interrupted = false;
  std::string error;

  int64_t end() const {
    return offset + static_cast<int64_t>(data.size() - head);
  }
  size_t ahead() const { return data.size() - head; }

  // Replaces the request with one from |from|. Under |mutex|.
  void Restart(int64_t from) {
    offset = from;
    data.clear();
    head = 0;
    discard = 0;
    complete = false;
    refused = false;
    error.clear();
    ++generation;
    if (request) {
      WinHttpCloseHandle(request);
      request = nullptr;
    }
    condition.notify_all();
  }

  // The thread: one request per generation, streamed into |data|.
  void Run() {
    std::vector<uint8_t> chunk(kChunk);
    std::unique_lock<std::mutex> lock(mutex);
    uint64_t served = 0;
    for (;;) {
      condition.wait(lock, [&] { return shutdown || generation != served; });
      if (shutdown) {
        return;
      }
      served = generation;
      // From 0 too, as FFmpeg does: a 206 says the server takes ranges.
      const std::wstring range =
          L"Range: bytes=" + std::to_wstring(offset) + L"-";
      lock.unlock();
      HINTERNET opened = WinHttpOpenRequest(
          connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
          WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
      const bool sent =
          opened &&
          WinHttpSendRequest(opened, range.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          WinHttpReceiveResponse(opened, nullptr);
      const DWORD failure = sent ? 0 : GetLastError();
      lock.lock();
      if (shutdown || generation != served) {
        if (opened) {
          WinHttpCloseHandle(opened);
        }
        continue;
      }
      if (!sent) {
        if (opened) {
          WinHttpCloseHandle(opened);
        }
        error = ErrorString(failure);
        complete = true;
        condition.notify_all();
        continue;
      }
      request = opened;
      Respond(opened);
      if (!complete) {
        Stream(opened, served, &chunk, &lock);
      }
      if (request == opened) {
        WinHttpCloseHandle(opened);
        request = nullptr;
      }
    }
  }

  // Reads the response's status and headers. Under |mutex|.
  void Respond(HINTERNET response) {
    DWORD status = 0;
    DWORD length = sizeof(status);
    WinHttpQueryHeaders(response,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                        WINHTTP_NO_HEADER_INDEX);
    if (status >= 400) {
      error = "HTTP " + std::to_string(status) + " " +
              Narrow(Header(response, WINHTTP_QUERY_STATUS_TEXT));
      // Server errors may pass; FFmpeg's reconnect_on_http_error=5xx.
      refused = status < 500;
      complete = true;
      condition.notify_all();
      return;
    }
    const std::wstring final_url = FinalUrl(response);
    if (!final_url.empty() && final_url != url) {
      location = Narrow(final_url);
    }
    ranges = status == 206 ||
             Header(response, WINHTTP_QUERY_ACCEPT_RANGES).find(L"bytes") !=
                 std::wstring::npos;
    if (status == 206) {
      // Content-Range: bytes first-last/total
      const std::wstring range = Header(response, WINHTTP_QUERY_CONTENT_RANGE);
      const size_t slash = range.find(L'/');
      if (slash != std::wstring::npos) {
        const int64_t total = _wcstoi64(range.c_str() + slash + 1, nullptr, 10);
        if (total > 0) {
          size = total;
        }
      }
    } else {
      // The whole resource, whatever was asked for.
      discard = offset;
      const std::wstring content_length =
          Header(response, WINHTTP_QUERY_CONTENT_LENGTH);
      if (!content_length.empty()) {
        size = _wcstoi64(content_length.c_str(), nullptr, 10);
      }
    }
  }

  // Reads the body of |response| for generation |served| until it ends,
  // fails or is replaced, at most kMaxAhead ahead of the reader. Holds
  // |lock| (on |mutex|) except while reading.
  void Stream(HINTERNET response,
              uint64_t served,
              std::vector<uint8_t>* chunk,
              std::unique_lock<std::mutex>* lock) {
    for (;;) {
      condition.wait(*lock, [&] {
        return shutdown || generation != served || ahead() < kMaxAhead;
      });
      if (shutdown || generation != served) {
        return;
      }
      lock->unlock();
      DWORD read = 0;
      const BOOL ok = WinHttpReadData(response, chunk->data(),
                                      static_cast<DWORD>(chunk->size()), &read);
      const DWORD failure = ok ? 0 : GetLastError();
      lock->lock();
      if (shutdown || generation != served) {
        return;
      }
      if (!ok || read == 0) {
        if (!ok && error.empty()) {
          error = ErrorString(failure);
        }
        complete = true;
        condition.notify_all();
        return;
      }
      const size_t skip = static_cast<size_t>(
          std::min<int64_t>(discard, static_cast<int64_t>(read)));
      discard -= static_cast<int64_t>(skip);
      data.insert(data.end(), chunk->begin() + static_cast<ptrdiff_t>(skip),
                  chunk->begin() + read);
      retries = 0;
      condition.notify_all();
    }
  }
};

std::unique_ptr<DataSource> WinHttpDataSource::Open(const std::string& url,
                                                    std::string* error) {
  auto state = std::make_unique<WinHttpState>();
  state->url = Widen(url);
  URL_COMPONENTS parts = {};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength =
      parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(state->url.c_str(), 0, 0, &parts) ||
      parts.dwHostNameLength == 0) {
    *error = "malformed URL";
    return nullptr;
  }
  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  state->path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
  state->path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  if (state->path.empty()) {
    state->path = L"/";
  }
  state->secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
  // The system's proxy settings, auto-configured ones included (Windows
  // 8.1 and later; WinHTTP's own setting before).
  state->session =
      WinHttpOpen(L"lychee", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!state->session) {
    state->session =
        WinHttpOpen(L"lychee", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (state->session) {
    WinHttpSetTimeouts(state->session, kIdleTimeoutMs, kIdleTimeoutMs,
                       kIdleTimeoutMs, kIdleTimeoutMs);
    state->connection =
        WinHttpConnect(state->session, host.c_str(), parts.nPort, 0);
  }
  if (!state->connection) {
    *error = ErrorString(GetLastError());
    if (state->session) {
      WinHttpCloseHandle(state->session);
    }
    return nullptr;
  }
  std::unique_ptr<WinHttpDataSource> source(
      new WinHttpDataSource(std::move(state)));
  {
    std::lock_guard<std::mutex> lock(source->state_->mutex);
    source->state_->Restart(0);
  }
  source->thread_ = std::thread(&WinHttpState::Run, source->state_.get());
  return source;
}

WinHttpDataSource::WinHttpDataSource(std::unique_ptr<WinHttpState> state)
    : state_(std::move(state)) {}

WinHttpDataSource::~WinHttpDataSource() {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->shutdown = true;
    if (state_->request) {
      WinHttpCloseHandle(state_->request);
      state_->request = nullptr;
    }
    state_->condition.notify_all();
  }
  // A request still being sent finishes or times out first.
  thread_.join();
  WinHttpCloseHandle(state_->connection);
  WinHttpCloseHandle(state_->session);
}

int64_t WinHttpDataSource::Read(int64_t offset, uint8_t* data, size_t size) {
  WinHttpState& state = *state_;
  std::unique_lock<std::mutex> lock(state.mutex);
  for (;;) {
    if (state.interrupted) {
      return -1;
    }
    if (state.size >= 0 && offset >= state.size) {
      return 0;
    }
    const int64_t end = state.end();
    if (offset >= state.offset && offset < end) {
      const size_t start =
          state.head + static_cast<size_t>(offset - state.offset);
      const size_t n = std::min(size, state.data.size() - start);
      memcpy(data, state.data.data() + start, n);
      // Drop what is well behind the reader.
      const size_t keep_from =
          start + n > kKeepBehind ? start + n - kKeepBehind : 0;
      if (keep_from > state.head) {
        state.offset += static_cast<int64_t>(keep_from - state.head);
        state.head = keep_from;
        if (state.head > state.data.size() / 2) {
          state.data.erase(
              state.data.begin(),
              state.data.begin() + static_cast<ptrdiff_t>(state.head));
          state.head = 0;
        }
      }
      // The thread may be waiting for room.
      state.condition.notify_all();
      return static_cast<int64_t>(n);
    }
    if (offset >= state.offset && offset <= end + kSkipAhead) {
      if (!state.complete) {
        state.condition.wait(lock);
        continue;
      }
      const bool ended =
          state.error.empty() && (state.size < 0 || end >= state.size);
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
        const int delay = (1 << state.retries) - 1;
        state.retries++;
        LOGW("reconnecting at %lld in %d s after: %s",
             static_cast<long long>(offset), delay,
             state.error.empty() ? "connection closed" : state.error.c_str());
        if (state.condition.wait_for(lock, std::chrono::seconds(delay),
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

bool WinHttpDataSource::seekable() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->ranges;
}

std::string WinHttpDataSource::url() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->location;
}

int64_t WinHttpDataSource::size() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->size;
}

void WinHttpDataSource::Interrupt() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->interrupted = true;
  state_->condition.notify_all();
}

void WinHttpDataSource::ClearInterrupt() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->interrupted = false;
}

bool WinHttpDataSource::interrupted() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->interrupted;
}

std::string WinHttpDataSource::error() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->interrupted ? "interrupted" : state_->error;
}

}  // namespace lychee
