#include "backends/ffmpeg/cleartext_redirect.h"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "backends/hls/hls_playlist.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/macros.h>
}

namespace lychee {
namespace {

// FFmpeg's http sends this too, so the probe and the request after it look
// the same to the server; some refuse requests without one.
constexpr char kUserAgent[] = "Lavf/" AV_STRINGIFY(LIBAVFORMAT_VERSION);

// host:port, or empty for a URL without a host.
std::string Origin(const std::string& url) {
  char host[4096];
  int port = -1;
  av_url_split(nullptr, 0, nullptr, 0, host, sizeof(host), &port, nullptr, 0,
               url.c_str());
  if (!host[0]) {
    return "";
  }
  return std::string(host) + ":" + std::to_string(port < 0 ? 80 : port);
}

// This only discovers redirects; FFmpeg's HTTP still handles nonredirects,
// authentication and streaming. Bound both headers and network waits.
int Redirect(const std::string& url,
             bool head,
             const AVIOInterruptCB& interrupt,
             std::string* location) {
  char host[4096], path[16384], auth[4096];
  int port = -1;
  av_url_split(nullptr, 0, auth, sizeof(auth), host, sizeof(host), &port, path,
               sizeof(path), url.c_str());
  if (auth[0] || !host[0] || url.find_first_of("\r\n") != std::string::npos ||
      url.size() >= sizeof(path)) {
    return 0;
  }
  // IPv6 literals need brackets in both TCP URLs and the Host header.
  std::string authority = std::string(host).find(':') == std::string::npos
                              ? host
                              : "[" + std::string(host) + "]";
  if (port < 0) {
    port = 80;
  }
  std::string endpoint = authority + ":" + std::to_string(port);
  AVDictionary* options = nullptr;
  av_dict_set(&options, "rw_timeout", "10000000", 0);
  av_dict_set(&options, "timeout", "10000000", 0);
  AVIOContext* io = nullptr;
  int ret = avio_open2(&io, ("tcp://" + endpoint).c_str(), AVIO_FLAG_READ_WRITE,
                       &interrupt, &options);
  av_dict_free(&options);
  if (ret < 0) {
    return 0;
  }
  std::string target = path[0] ? path : "/";
  if (target[0] == '?') {
    target = "/" + target;
  }
  target = target.substr(0, target.find('#'));
  std::string request =
      std::string(head ? "HEAD " : "GET ") + target +
      " HTTP/1.1\r\nHost: " + endpoint + "\r\nUser-Agent: " + kUserAgent +
      "\r\nConnection: close\r\nAccept-Encoding: identity\r\n" +
      (head ? "" : "Range: bytes=0-0\r\n") + "\r\n";
  avio_write(io, reinterpret_cast<const unsigned char*>(request.data()),
             static_cast<int>(request.size()));
  avio_flush(io);
  // A read/write AVIO is otherwise a buffered writer; read the reply directly.
  io->direct = 1;
  std::string headers;
  uint8_t byte;
  while (headers.size() < 65536 && avio_read(io, &byte, 1) == 1) {
    headers += static_cast<char>(byte);
    if (headers.size() >= 4 &&
        headers.compare(headers.size() - 4, 4, "\r\n\r\n") == 0) {
      break;
    }
  }
  avio_closep(&io);
  const size_t code = headers.find(' ');
  if (headers.size() < 4 || headers.rfind("HTTP/", 0) != 0 ||
      headers.compare(headers.size() - 4, 4, "\r\n\r\n") != 0 ||
      code == std::string::npos) {
    return 0;
  }
  int status = 0;
  const char* begin_code = headers.data() + code + 1;
  const char* end_headers = headers.data() + headers.size();
  const auto parsed = std::from_chars(begin_code, end_headers, status);
  if (parsed.ec != std::errc{} || parsed.ptr - begin_code != 3 ||
      parsed.ptr == end_headers ||
      (*parsed.ptr != ' ' && *parsed.ptr != '\r')) {
    return 0;
  }
  for (size_t begin = headers.find("\r\n") + 2; begin < headers.size();) {
    size_t end = headers.find("\r\n", begin);
    if (end == std::string::npos || end == begin) {
      break;
    }
    std::string line = headers.substr(begin, end - begin);
    size_t colon = line.find(':');
    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
      return static_cast<char>(tolower(c));
    });
    if (name == "location" && colon != std::string::npos) {
      const size_t first = line.find_first_not_of(" \t", colon + 1);
      const size_t last = line.find_last_not_of(" \t");
      if (first != std::string::npos) {
        *location = ResolveUrl(url, line.substr(first, last - first + 1));
      }
    }
    begin = end + 2;
  }
  return status;
}

}  // namespace

std::string CleartextRedirects::Find(const std::string& url,
                                     const AVIOInterruptCB& interrupt,
                                     std::string* error) {
  std::string current = url;
  for (int hops = 0; hops <= 5; ++hops) {
    if (current.rfind("https://", 0) == 0) {
      return current;
    }
    if (current.rfind("http://", 0) != 0) {
      *error = "HTTP redirect uses an unsupported scheme";
      return "";
    }
    std::string location;
    int status = Redirect(current, true, interrupt, &location);
    if (status == 405 || status == 501) {
      status = Redirect(current, false, interrupt, &location);
    }
    if ((status != 301 && status != 302 && status != 303 && status != 307 &&
         status != 308) ||
        location.empty()) {
      // FFmpeg reads it, following any HTTP redirects on the way.
      std::lock_guard<std::mutex> lock(mutex_);
      plain_.insert(Origin(url));
      return "";
    }
    if (hops == 5) {
      *error = "HTTP redirect limit exceeded (5)";
      return "";
    }
    current = std::move(location);
  }
  return "";
}

bool CleartextRedirects::Known(const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  return plain_.count(Origin(url)) != 0;
}

void CleartextRedirects::Forget(const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  plain_.erase(Origin(url));
}

}  // namespace lychee
