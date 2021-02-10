//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_CLOCK_H
#define FFPLAYER_FFP_CLOCK_H


extern "C" {
#include <libavutil/time.h>
};

/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

struct Clock {
    double last_updated;
    int serial; /* clock is based on a packet with this serial */
    int paused = 0;

private:
    double speed_ = 1.0;
    int *queue_serial_; /* pointer to the current packet queue serial, used for obsolete clock detection */
    double pts_;       /* clock base */
    double pts_drift_; /* clock base minus time at which we updated the clock */

public:
    void Init(int *queue_serial);

    double GetClock();

    void SetClockAt(double pts, int serial, double time);

    void SetClock(double pts, int serial);

    void SetSpeed(double speed);

    double GetSpeed() const;

    void Sync(Clock *secondary);

};

#endif //FFPLAYER_FFP_CLOCK_H
