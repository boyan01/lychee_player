//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_DATA_SOURCE_H_
#define MEDIA_PLAYER_DATA_SOURCE_H_

#include <functional>

#include "base/basictypes.h"

namespace media {

/**
 * Object which host datasource.
 */
class DataSourceHost {

 public:
  /**
   * Set the total size of the media file.
   */
  virtual void SetTotalBytes(int64 total_bytes) = 0;

  /**
   * Notify the host that byte range [start, end] has been buffered.
   */
  virtual void AddBufferedByteRange(int64 start, int64 end) = 0;

  /**
   * Notify the host that time range `start`,`end` has been buffered.
   */
  virtual void AddBufferedTimeRange(chrono::microseconds start, chrono::microseconds end) = 0;

 protected:
  virtual ~DataSourceHost();

};

class DataSource {

 public:

  typedef std::function<void(int64, int64)> StatusCallback;
  typedef std::function<void(int)> ReadCB;
  static const int kReadError;

  void SetHost(DataSourceHost *host);

  // Notify the DataSource of the bitrate of the media.
  // Values of |bitrate| <= 0 are invalid and should be ignored.
  virtual void SetBitrate(int bitrate) = 0;

  // Reads |size| bytes from |position| into |data|. And when the read is done
  // or failed, |read_cb| is called with the number of bytes read or
  // kReadError in case of error.
  virtual void Read(int64 position, int size, uint8 *data,
                    const DataSource::ReadCB &read_cb) = 0;

  // Notifies the DataSource of a change in the current playback rate.
  virtual void SetPlaybackRate(float playback_rate);

  // Stops the DataSource. Once this is called all future Read() calls will
  // return an error.
  virtual void Stop(const std::function<void()> &callback) = 0;

  // Returns true and the file size, false if the file size could not be
  // retrieved.
  virtual bool GetSize(int64 *size_out) = 0;

 protected:

  virtual ~DataSource();

  DataSourceHost *host();

 private:
  DataSourceHost *host_;

  DISALLOW_COPY_AND_ASSIGN(DataSource);

};

}

#endif //MEDIA_PLAYER_DATA_SOURCE_H_
