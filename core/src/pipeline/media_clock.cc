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
  if (MovingLocked()) {
    media_time += std::llround((system_time - anchor_system_time_) * rate_);
  }
  return std::clamp(media_time, std::min(min_media_time_, max_media_time_),
                    max_media_time_);
}

bool MediaClock::MovingLocked() const {
  return playing_ && !waiting_for_audio_;
}

bool MediaClock::SystemTimesFor(const std::vector<int64_t>& media_times,
                                std::vector<int64_t>* system_times) const {
  int64_t now = NowMicros();
  std::lock_guard<std::mutex> lock(mutex_);
  const bool moving = MovingLocked();
  int64_t anchor_system_time = moving ? anchor_system_time_ : now;
  system_times->clear();
  for (int64_t media_time : media_times) {
    system_times->push_back(
        anchor_system_time +
        std::llround((media_time - anchor_media_time_) / rate_));
  }
  return moving;
}

void MediaClock::Reset(int64_t media_time) {
  std::lock_guard<std::mutex> lock(mutex_);
  anchor_media_time_ = media_time;
  anchor_system_time_ = NowMicros();
  max_media_time_ = kInfiniteTime;
  min_media_time_ = media_time;
  playing_ = false;
  waiting_for_audio_ = false;
  has_audio_anchor_ = false;
}

void MediaClock::Play(bool wait_for_audio) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (playing_) {
    return;
  }
  min_media_time_ = anchor_media_time_;
  anchor_system_time_ = NowMicros();
  waiting_for_audio_ = wait_for_audio;
  playing_ = true;
}

void MediaClock::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!playing_) {
    return;
  }
  anchor_media_time_ = MediaTimeAtLocked(NowMicros());
  playing_ = false;
  waiting_for_audio_ = false;
  has_audio_anchor_ = false;
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
  // A discontinuity in the samples (for example at a loop boundary) is
  // intentional. Only slew device timing changes for contiguous audio.
  // Timestamp rounding at non-integer rates can differ by one microsecond.
  if (has_audio_anchor_ && max_media_time_ != kInfiniteTime &&
      std::llabs(media_time - max_media_time_) <= 1) {
    const int64_t now = NowMicros();
    const int64_t previous_now =
        anchor_media_time_ + std::llround((now - anchor_system_time_) * rate_);
    // After an underrun, the old device timeline has expired: the next
    // buffer starts where it stopped. Do not smooth that fresh start.
    if (previous_now < max_media_time_) {
      const int64_t predicted =
          anchor_media_time_ +
          std::llround((system_time - anchor_system_time_) * rate_);
      // Bound phase correction in media time (one millisecond at 1x).
      // 200 ms of route latency converges in 200 callbacks; normal device
      // drift is far smaller than this allowance. Compare unclamped times
      // so buffers queued before the first audible sample keep its latency.
      const int64_t limit =
          std::max<int64_t>(1, std::llround(kMicrosPerMilli * rate_));
      media_time =
          predicted + std::clamp(media_time - predicted, -limit, limit);
    }
  }
  anchor_media_time_ = media_time;
  anchor_system_time_ = system_time;
  max_media_time_ = max_media_time;
  waiting_for_audio_ = false;
  has_audio_anchor_ = true;
}

void MediaClock::ClearMaxTime() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (max_media_time_ == kInfiniteTime && !waiting_for_audio_) {
    return;
  }
  // Continue from where audio stopped instead of jumping ahead.
  int64_t now = NowMicros();
  anchor_media_time_ = MediaTimeAtLocked(now);
  anchor_system_time_ = now;
  max_media_time_ = kInfiniteTime;
  waiting_for_audio_ = false;
  has_audio_anchor_ = false;
}

}  // namespace lychee
