#include "pipeline/media_clock.h"

#include <algorithm>
#include <cmath>

namespace lychee {

int64_t MediaClock::MediaTimeAt(int64_t system_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return MediaTimeAtLocked(system_time);
}

int64_t MediaClock::MediaTimeAtLocked(int64_t system_time) const {
  int64_t media_time = anchor_media_time_;
  if (playing_) {
    media_time += std::llround((system_time - anchor_system_time_) * rate_);
  }
  return std::min(media_time, max_media_time_);
}

bool MediaClock::SystemTimesFor(const std::vector<int64_t>& media_times,
                                std::vector<int64_t>* system_times) const {
  int64_t now = NowMicros();
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t anchor_system_time = playing_ ? anchor_system_time_ : now;
  system_times->clear();
  for (int64_t media_time : media_times) {
    system_times->push_back(
        anchor_system_time +
        std::llround((media_time - anchor_media_time_) / rate_));
  }
  return playing_;
}

void MediaClock::Reset(int64_t media_time) {
  std::lock_guard<std::mutex> lock(mutex_);
  anchor_media_time_ = media_time;
  anchor_system_time_ = NowMicros();
  max_media_time_ = kInfiniteTime;
  playing_ = false;
}

void MediaClock::Play() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (playing_) {
    return;
  }
  anchor_system_time_ = NowMicros();
  playing_ = true;
}

void MediaClock::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!playing_) {
    return;
  }
  anchor_media_time_ = MediaTimeAtLocked(NowMicros());
  playing_ = false;
}

bool MediaClock::playing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return playing_;
}

void MediaClock::SetRate(double rate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rate <= 0 || rate == rate_) {
    return;
  }
  // Re-anchor so the media time already elapsed keeps the old rate.
  int64_t now = NowMicros();
  anchor_media_time_ = MediaTimeAtLocked(now);
  anchor_system_time_ = now;
  rate_ = rate;
}

double MediaClock::rate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rate_;
}

void MediaClock::UpdateFromAudio(int64_t media_time,
                                 int64_t system_time,
                                 int64_t max_media_time) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!playing_) {
    return;
  }
  anchor_media_time_ = media_time;
  anchor_system_time_ = system_time;
  max_media_time_ = max_media_time;
}

void MediaClock::ClearMaxTime() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (max_media_time_ == kInfiniteTime) {
    return;
  }
  // Continue from where audio stopped instead of jumping ahead.
  int64_t now = NowMicros();
  anchor_media_time_ = MediaTimeAtLocked(now);
  anchor_system_time_ = now;
  max_media_time_ = kInfiniteTime;
}

}  // namespace lychee
