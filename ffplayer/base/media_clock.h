//
// Created by boyan on 2021/2/10.
//

#ifndef BASE_MEDIA_CLOCK_H
#define BASE_MEDIA_CLOCK_H

#include <memory>
#include <functional>

extern "C" {
#include <libavutil/time.h> // NOLINT(modernize-deprecated-headers)
};

/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

enum {
  AV_SYNC_AUDIO_MASTER, /* default choice */
  AV_SYNC_VIDEO_MASTER,
  AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

struct Clock {
  double last_updated = 0;
  int serial = 0; /* clock is based on a packet with this serial */
  int paused = 0;

 private:
  double speed_ = 1.0;

  /* pointer to the current packet queue serial, used for obsolete clock detection */
  int *queue_serial_;

  /* clock base */
  double pts_;

  /* clock base minus time at which we updated the clock */
  double pts_drift_;

 public:

  explicit Clock(int *queue_serial);

  explicit Clock();

  double GetClock();

  void SetClockAt(double pts, int serial, double time);

  void SetClock(double pts, int serial);

  void SetSpeed(double speed);

  double GetSpeed() const;

  void Sync(Clock *secondary);

};

class MediaClock {
 private:
  int av_sync_type_ = AV_SYNC_AUDIO_MASTER;

  std::unique_ptr<Clock> audio_clock_;
  std::unique_ptr<Clock> video_clock_;
  std::unique_ptr<Clock> ext_clock_;

  std::function<int(int sync_type)> sync_type_confirm_;

 public:

  MediaClock(int *audio_queue_serial, int *video_queue_serial, std::function<int(int)> sync_type_confirm);

  Clock *GetAudioClock();

  Clock *GetVideoClock();

  Clock *GetExtClock();

  int GetMasterSyncType() const;

  double GetMasterClock();

};

#endif //BASE_MEDIA_CLOCK_H
