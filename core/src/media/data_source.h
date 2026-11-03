#ifndef LYCHEE_MEDIA_DATA_SOURCE_H_
#define LYCHEE_MEDIA_DATA_SOURCE_H_

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace lychee {

// Random access bytes of a file or URL: what Mp4Demuxer parses, and what
// FFmpeg reads on Apple platforms and Android (ffmpeg_demuxer.cc), so both
// builds share one http client.
class DataSource {
 public:
  virtual ~DataSource() = default;

  // Blocking. Reads up to |size| bytes at |offset|. Returns the bytes
  // read, 0 at the end of the data, or -1 on errors and interrupts.
  virtual int64_t Read(int64_t offset, uint8_t* data, size_t size) = 0;

  // Total size in bytes, or -1 if unknown.
  virtual int64_t size() const = 0;

  // Whether a read elsewhere than where the last one ended is cheap. A
  // server without range requests can still be read at any offset, but
  // from the start again. Known once a Read() returned.
  virtual bool seekable() const { return true; }

  // The URL after redirects, known once a Read() returned; empty if there
  // were none.
  virtual std::string url() const { return ""; }

  // Whether the last Read() failed because the platform refuses the URL
  // as it is: cleartext http under App Transport Security, or Android's
  // network security config, in an app without an exception. A client
  // of its own (FFmpeg's) may still read it.
  virtual bool cleartext_refused() const { return false; }

  // Thread-safe. Makes the current and later Read() calls return -1 until
  // ClearInterrupt().
  virtual void Interrupt() {}
  virtual void ClearInterrupt() {}
  virtual bool interrupted() const { return false; }

  // Why the last Read() failed.
  virtual std::string error() const = 0;

  // Bits per second the bytes so far arrived at, while being sent (from
  // a request to its last bytes), or -1 if unknown: what HLS adapts its
  // variant to.
  virtual int64_t transfer_rate() const { return -1; }

  // Whether its bytes come without waiting once it is open: a local file,
  // or a client that downloads ahead of the reader by itself (Apple's,
  // Windows'). Others connect on their first Read(), so opening one early
  // fetches nothing (CreatePrefetchingDataSource()).
  virtual bool reads_ahead() const { return false; }
};

// Blocking. A path or file:// URL, or http(s):// where the platform has a
// client (Apple, Windows; Android with a Java VM) or FFmpeg's (Linux). Null
// with |error| set if it cannot be opened.
std::unique_ptr<DataSource> OpenDataSource(const std::string& url,
                                           std::string* error);

// Whether media read from |base| may make the player open |url|, a
// reference it resolved (an HLS segment, key or variant, an IMSC
// image): media from the network reaches only the network, and no media
// reaches a Windows UNC path (//host/share) on a host other than its
// own, which Windows would log in to with the user's credentials. The
// caller's own URLs are not references. Inline, as the Android FFmpeg
// extension compiles no data_source.cc.
inline bool MayOpenReference(const std::string& base, const std::string& url) {
  const auto http = [](std::string_view value) {
    return value.starts_with("http://") || value.starts_with("https://");
  };
  if (http(url)) {
    return true;
  }
  if (http(base)) {
    return false;
  }
  // The host of the UNC path a local URL opens (OpenDataSource drops
  // file://), lowercase, or empty for another path.
  const auto unc_host = [](std::string_view path) {
    if (path.starts_with("file://")) {
      path.remove_prefix(7);
    }
    const auto separator = [](char c) { return c == '/' || c == '\\'; };
    if (path.size() < 3 || !separator(path[0]) || !separator(path[1])) {
      return std::string();
    }
    path.remove_prefix(2);
    std::string host(path.substr(0, path.find_first_of("/\\")));
    for (char& c : host) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return host;
  };
  const std::string host = unc_host(url);
  // \\?\ and \\.\ are device paths, whatever follows them.
  return host.empty() || (host != "?" && host != "." && host == unc_host(base));
}

using DataSourceOpener =
    std::function<std::unique_ptr<DataSource>(const std::string& url,
                                              std::string* error)>;
// OpenDataSource, but the http(s) sources it opens share connections:
// one NSURLSession on Apple platforms, one WinHTTP session on Windows,
// so that the playlists and segments of an HLS stream reuse kept-alive
// connections instead of a new TCP and TLS handshake each. Elsewhere
// OpenDataSource itself.
DataSourceOpener CreateSharedOpener();

// Bytes in memory (tests, the fuzzer).
std::unique_ptr<DataSource> CreateMemoryDataSource(std::vector<uint8_t> data);

// |source|, with a thread of its own that reads its first |limit| bytes
// into memory from now on: HLS's next segment while the current one plays.
// Reads wait for those bytes; past them, or once the thread stopped
// early, they go to |source|. Its other calls answer from the thread's
// last read until it is done.
std::unique_ptr<DataSource> CreatePrefetchingDataSource(
    std::unique_ptr<DataSource> source,
    size_t limit);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_DATA_SOURCE_H_
