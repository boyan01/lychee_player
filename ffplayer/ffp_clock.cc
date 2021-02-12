//
// Created by boyan on 2021/2/10.
//

#include <cmath>

#include "ffp_clock.h"

void Clock::Init(int *queue_serial) {
    speed_ = 1.0;
    paused = 0;
    queue_serial_ = queue_serial;
    SetClock(NAN, -1);
}

void Clock::SetClockAt(double pts, int _serial, double time) {
    pts = pts;
    last_updated = time;
    pts_drift_ = pts - time;
    serial = _serial;
}

void Clock::SetClock(double pts, int _serial) {
    double time = av_gettime_relative() / 1000000.0;
    SetClockAt(pts, _serial, time);
}

void Clock::SetSpeed(double speed) {
    SetClock(GetClock(), serial);
    speed_ = speed;
}

void Clock::Sync(Clock *secondary) {
    double clock = GetClock();
    double secondary_clock = secondary->GetClock();
    if (!std::isnan(secondary_clock) && (std::isnan(clock) || fabs(clock - secondary_clock) > AV_NOSYNC_THRESHOLD)) {
        SetClock(secondary_clock, secondary->serial);
    }
}

double Clock::GetClock() {
    if (*queue_serial_ != serial)
        return NAN;
    if (paused) {
        return pts_;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return pts_drift_ + time - (time - last_updated) * (1.0 - speed_);
    }
}

double Clock::GetSpeed() const {
    return speed_;
}

Clock *ClockContext::GetVideoClock() {
    return video_clock_.get();
}

Clock *ClockContext::GetAudioClock() {
    return audio_clock_.get();
}

Clock *ClockContext::GetExtClock() {
    return ext_clock_.get();
}

int ClockContext::GetMasterSyncType() const {
    if (sync_type_confirm_) {
        return sync_type_confirm_(av_sync_type_);
    } else {
        return av_sync_type_;
    }
}

double ClockContext::GetMasterClock() {
    switch (GetMasterSyncType()) {
        case AV_SYNC_AUDIO_MASTER:
            return audio_clock_->GetClock();
        case AV_SYNC_VIDEO_MASTER:
            return video_clock_->GetClock();
        default:
            return ext_clock_->GetClock();
    }
}
