//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_BUFFER_H_
#define MEDIA_PLAYER_BUFFER_H_

#include "base/basictypes.h"
#include "base/timestamps.h"

namespace media {

extern inline TimeDelta kNoTimestamp() {
  return TimeDelta::min();
}

extern inline TimeDelta kInfiniteDuration() {
  return TimeDelta::max();
}

class Buffer {

 public:
  /**
   * @return A read only pointer to the buffer data.
   */
  virtual const uint8 *GetData() const = 0;

  /**
   * @return The size of valid data in bytes.
   */
  virtual int GetDataSize() const = 0;

  /**
   * @return true if no data in this buffer, end of stream.
   */
  bool isEndOfStream() const;

  TimeDelta GetTimestamp() const {
    return timestamp_;
  }

  void SetTimeStamp(const TimeDelta &timestamp) {
    timestamp_ = timestamp;
  }

  const TimeDelta &GetDuration() const {
    return duration_;
  }
  void SetDuration(const TimeDelta &duration) {
    duration_ = duration;
  }

 protected:
  Buffer(const TimeDelta &timestamp, const TimeDelta &duration);
  virtual ~Buffer();

 private:
  TimeDelta timestamp_;
  TimeDelta duration_;

  DISALLOW_COPY_AND_ASSIGN(Buffer);

};

} // namespace media
#endif //MEDIA_PLAYER_BUFFER_H_
