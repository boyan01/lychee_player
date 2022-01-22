//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_SOURCE_DATA_SOURCE_H_
#define MEDIA_PLAYER_SOURCE_DATA_SOURCE_H_

#include "base/basictypes.h"
#include "functional"

namespace media {

class DataSource {
 public:
  using ReadCB = std::function<void(int)>;

  enum { kReadError = -1, kAborted = -2 };

  DataSource() = default;
  virtual ~DataSource() = default;

  // Reads |size| bytes from |position| into |data|. And when the read is done
  // or failed, |read_cb| is called with the number of bytes read or
  // kReadError in case of error.
  virtual void Read(int64_t position, int size, uint8_t *data,
                    DataSource::ReadCB read_cb) = 0;

  // Stops the DataSource. Once this is called all future Read() calls will
  // return an error. This is a synchronous call and may be called from any
  // thread. Once called, the DataSource may no longer be used and should be
  // destructed shortly thereafter.
  virtual void Stop() = 0;

  // Similar to Stop(), but only aborts current reads and not future reads.
  virtual void Abort() = 0;

  // Returns true and the file size, false if the file size could not be
  // retrieved.
  virtual bool GetSize(int64_t *size_out) = 0;

  // Returns true if we are performing streaming. In this case seeking is
  // not possible.
  virtual bool IsStreaming() = 0;

  // Notify the DataSource of the bitrate of the media.
  // Values of |bitrate| <= 0 are invalid and should be ignored.
  virtual void SetBitrate(int bitrate) = 0;

  // Assume fully buffered by default.
  virtual bool AssumeFullyBuffered() const { return true; }

  // By default this just returns GetSize().
  virtual int64_t GetMemoryUsage() {
    int64_t temp;
    return GetSize(&temp) ? temp : 0;
  }

 private:
  DELETE_COPY_AND_ASSIGN(DataSource);
};

}  // namespace media

#endif  // MEDIA_PLAYER_SOURCE_DATA_SOURCE_H_
