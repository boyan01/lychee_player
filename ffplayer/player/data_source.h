//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_DATA_SOURCE_H_
#define MEDIA_PLAYER_DATA_SOURCE_H_

#include <functional>

#include "basictypes.h"

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

 protected:

  virtual ~DataSource();

  DataSourceHost *host();

 private:
  DataSourceHost *host_;

  DISALLOW_COPY_AND_ASSIGN(DataSource);

};

}

#endif //MEDIA_PLAYER_DATA_SOURCE_H_
