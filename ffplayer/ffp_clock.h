//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_CLOCK_H
#define FFPLAYER_FFP_CLOCK_H

#include <memory>
#include <functional>

extern "C" {
#include <libavutil/time.h>
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
    int *queue_serial_; /* pointer to the current packet queue serial, used for obsolete clock detection */
    double pts_ = 0;       /* clock base */
    double pts_drift_ = 0; /* clock base minus time at which we updated the clock */

public:
    void Init(int *queue_serial);

    double GetClock();

    void SetClockAt(double pts, int serial, double time);

    void SetClock(double pts, int serial);

    void SetSpeed(double speed);

    double GetSpeed() const;

    void Sync(Clock *secondary);

};

class ClockContext {
private:
    int av_sync_type_ = AV_SYNC_AUDIO_MASTER;

    const std::unique_ptr<Clock> audio_clock_;
    const std::unique_ptr<Clock> video_clock_;
    const std::unique_ptr<Clock> ext_clock_;

    std::function<int(int sync_type)> sync_type_confirm_;

public:

    ClockContext() : audio_clock_(new Clock), video_clock_(new Clock), ext_clock_(new Clock) {}

    void Init(int *queue_serial, std::function<int(int sync_type)> sync_type_confirm) {
        audio_clock_->Init(queue_serial);
        video_clock_->Init(queue_serial);
        ext_clock_->Init(queue_serial);
        sync_type_confirm_ = sync_type_confirm;
    }

    Clock *GetAudioClock();

    Clock *GetVideoClock();

    Clock *GetExtClock();

    int GetMasterSyncType() const;

    double GetMasterClock();

};

#endif //FFPLAYER_FFP_CLOCK_H
