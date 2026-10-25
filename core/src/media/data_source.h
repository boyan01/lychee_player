#ifndef LYCHEE_MEDIA_DATA_SOURCE_H_
#define LYCHEE_MEDIA_DATA_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lychee {

// Random access bytes of a file or URL, for demuxers that parse containers
// themselves (Mp4Demuxer). The FFmpeg demuxer has its own I/O.
class DataSource {
 public:
  virtual ~DataSource() = default;

  // Blocking. Reads up to |size| bytes at |offset|. Returns the bytes
  // read, 0 at the end of the data, or -1 on errors and interrupts.
  virtual int64_t Read(int64_t offset, uint8_t* data, size_t size) = 0;

  // Total size in bytes, or -1 if unknown.
  virtual int64_t size() const = 0;

  // Thread-safe. Makes the current and later Read() calls return -1 until
  // ClearInterrupt().
  virtual void Interrupt() {}
  virtual void ClearInterrupt() {}
  virtual bool interrupted() const { return false; }

  // Why the last Read() failed.
  virtual std::string error() const = 0;
};

// Blocking. A path or file:// URL, or http(s):// where the platform has a
// client (Apple). Null with |error| set if it cannot be opened.
std::unique_ptr<DataSource> OpenDataSource(const std::string& url,
                                           std::string* error);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_DATA_SOURCE_H_
