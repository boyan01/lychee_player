//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_DATA_SOURCE_H_
#define MEDIA_PLAYER_DATA_SOURCE_H_

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

  void SetHost(DataSourceHost *host);

 protected:

  virtual ~DataSource();

  DataSourceHost *host();

 private:
  DataSourceHost *host_;

  DISALLOW_COPY_AND_ASSIGN(DataSource);

};

}

#endif //MEDIA_PLAYER_DATA_SOURCE_H_
