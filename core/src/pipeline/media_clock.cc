#include "pipeline/media_clock.h"

#include <algorithm>
#include <cmath>

namespace lychee {

int64_t MediaClock::Anchor::MediaTimeAt(int64_t time) const {
  return media_time + std::llround((time - system_time) * rate);
}

int64_t MediaClock::Anchor::SystemTimeFor(int64_t time) const {
  return system_time + std::llround((time - media_time) / rate);
}

const MediaClock::Anchor& MediaClock::AnchorAtLocked(
    int64_t system_time) const {
  for (const auto& change : rate_changes_) {
    if (system_time < change.system_time) {
      return change.previous;
    }
  }
  return anchor_;
}

int64_t MediaClock::MediaTimeAt(int64_t system_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return MediaTimeAtLocked(system_time);
}

int64_t MediaClock::MediaTimeAtLocked(int64_t system_time) const {
  const bool moving = MovingLocked();
  const Anchor& anchor = moving ? AnchorAtLocked(system_time) : anchor_;
  const int64_t media_time =
      moving ? anchor.MediaTimeAt(system_time) : anchor.media_time;
  return std::clamp(media_time,
                    std::min(min_media_time_, anchor.max_media_time),
                    anchor.max_media_time);
}

bool MediaClock::MovingLocked() const {
  return playing_ && !waiting_for_audio_;
}

bool MediaClock::SystemTimesFor(const std::vector<int64_t>& media_times,
                                std::vector<int64_t>* system_times) const {
  int64_t now = NowMicros();
  std::lock_guard<std::mutex> lock(mutex_);
  // Audio can run out while Play() remains in effect. Extrapolating that
  // expired anchor makes frame selection advance past the held position.
  const int64_t media_now = MediaTimeAtLocked(now);
  const Anchor& active = AnchorAtLocked(now);
  const bool moving = MovingLocked() && media_now < active.max_media_time;
  const Anchor stationary{media_now, now, kInfiniteTime, active.rate};
  system_times->clear();
  for (int64_t media_time : media_times) {
    const Anchor* anchor = &stationary;
    if (moving) {
      anchor = &anchor_;
      for (const auto& change : rate_changes_) {
        if (now < change.system_time && media_time < change.media_time) {
          anchor = &change.previous;
          break;
        }
      }
    }
    system_times->push_back(anchor->SystemTimeFor(media_time));
  }
  return moving;
}

void MediaClock::Reset(int64_t media_time) {
  std::lock_guard<std::mutex> lock(mutex_);
  anchor_ = {media_time, NowMicros(), kInfiniteTime, anchor_.rate};
  rate_changes_.clear();
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
  min_media_time_ = anchor_.media_time;
  anchor_.system_time = NowMicros();
  waiting_for_audio_ = wait_for_audio;
  playing_ = true;
}

void MediaClock::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!playing_) {
    return;
  }
  anchor_.media_time = MediaTimeAtLocked(NowMicros());
  rate_changes_.clear();
  playing_ = false;
  waiting_for_audio_ = false;
  has_audio_anchor_ = false;
}

bool MediaClock::playing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return playing_;
}

bool MediaClock::moving() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return MovingLocked();
}

void MediaClock::SetRate(double rate, int64_t system_time) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (rate <= 0 || rate == anchor_.rate) {
    return;
  }
  const int64_t now = NowMicros();
  const int64_t at = std::max(system_time, now);
  const int64_t media_time = MediaTimeAtLocked(at);
  DiscardPlayedRateChangesLocked(now);
  SavePreviousRateLocked(at, media_time, now);
  anchor_ = {media_time, at, anchor_.max_media_time, rate};
}

double MediaClock::rate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return anchor_.rate;
}

void MediaClock::DiscardPlayedRateChangesLocked(int64_t now) {
  while (!rate_changes_.empty() && rate_changes_.front().system_time <= now) {
    rate_changes_.pop_front();
  }
}

void MediaClock::SavePreviousRateLocked(int64_t system_time,
                                        int64_t media_time,
                                        int64_t now) {
  const Anchor previous = AnchorAtLocked(system_time);
  // A route change can deliver an earlier presentation time.
  while (!rate_changes_.empty() &&
         rate_changes_.back().system_time >= system_time) {
    rate_changes_.pop_back();
  }
  if (system_time > now && MovingLocked()) {
    rate_changes_.push_back({system_time, media_time, previous});
  }
}

void MediaClock::UpdateFromAudio(int64_t media_time,
                                 int64_t system_time,
                                 int64_t max_media_time,
                                 double rate) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!playing_) {
    return;
  }
  // A discontinuity in the samples (for example at a loop boundary) is
  // intentional. Only slew device timing changes for contiguous audio.
  // Timestamp rounding at non-integer rates can differ by one microsecond.
  const int64_t now = NowMicros();
  DiscardPlayedRateChangesLocked(now);
  if (rate == 0) {
    rate = anchor_.rate;
  }
  if (has_audio_anchor_ && anchor_.max_media_time != kInfiniteTime &&
      std::llabs(media_time - anchor_.max_media_time) <= 1) {
    // After an underrun, the old device timeline has expired: the next
    // buffer starts where it stopped. Do not smooth that fresh start.
    if (anchor_.MediaTimeAt(now) < anchor_.max_media_time) {
      const int64_t predicted = anchor_.MediaTimeAt(system_time);
      // Bound phase correction in media time (one millisecond at 1x).
      // 200 ms of route latency converges in 200 callbacks; normal device
      // drift is far smaller than this allowance. Compare unclamped times
      // so buffers queued before the first audible sample keep its latency.
      const int64_t limit =
          std::max<int64_t>(1, std::llround(kMicrosPerMilli * anchor_.rate));
      media_time =
          predicted + std::clamp(media_time - predicted, -limit, limit);
    }
  }
  if (rate != anchor_.rate) {
    SavePreviousRateLocked(system_time, media_time, now);
  }
  anchor_ = {media_time, system_time, max_media_time, rate};
  waiting_for_audio_ = false;
  has_audio_anchor_ = true;
}

void MediaClock::ClearMaxTime() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (anchor_.max_media_time == kInfiniteTime && !waiting_for_audio_) {
    return;
  }
  // Preserve the playout anchor while the device is still playing queued
  // samples, even if the first sample has yet to reach the device.
  // If audio has already stopped or never started, continue from now.
  const int64_t now = NowMicros();
  const int64_t media_now = MediaTimeAtLocked(now);
  if (waiting_for_audio_ || media_now >= anchor_.max_media_time) {
    anchor_.media_time = media_now;
    anchor_.system_time = now;
  }
  anchor_.max_media_time = kInfiniteTime;
  waiting_for_audio_ = false;
  has_audio_anchor_ = false;
}

}  // namespace lychee
