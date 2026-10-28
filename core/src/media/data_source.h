#ifndef LYCHEE_MEDIA_DATA_SOURCE_H_
#define LYCHEE_MEDIA_DATA_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
};

// Blocking. A path or file:// URL, or http(s):// where the platform has a
// client (Apple, Windows; Android with a Java VM) or FFmpeg's (Linux). Null
// with |error| set if it cannot be opened.
std::unique_ptr<DataSource> OpenDataSource(const std::string& url,
                                           std::string* error);

// Bytes in memory (tests, the fuzzer).
std::unique_ptr<DataSource> CreateMemoryDataSource(std::vector<uint8_t> data);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_DATA_SOURCE_H_
