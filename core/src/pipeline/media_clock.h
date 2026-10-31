#ifndef LYCHEE_PIPELINE_MEDIA_CLOCK_H_
#define LYCHEE_PIPELINE_MEDIA_CLOCK_H_

#include <cstdint>
#include <mutex>
#include <vector>

#include "base/time.h"

namespace lychee {

// Maps system time (NowMicros() timebase) to media time. Driven by the
// system clock, or by the audio renderer while audio is playing. Media time
// advances |rate| times as fast as system time.
class MediaClock {
 public:
  int64_t MediaTimeAt(int64_t system_time) const;
  int64_t Now() const { return MediaTimeAt(NowMicros()); }
  // The system times at which |media_times| play, as if playing from now on
  // while paused, waiting for audio, or after its last supplied sample.
  // Returns whether time is moving.
  bool SystemTimesFor(const std::vector<int64_t>& media_times,
                      std::vector<int64_t>* system_times) const;

  // Pauses the clock at |media_time|.
  void Reset(int64_t media_time);
  // With |wait_for_audio|, media time stays put until the audio renderer
  // reports when its first buffer is heard (or that audio ended), instead of
  // running ahead by the output latency and jumping back: video would skip the
  // frames of that gap. Chromium likewise reports time as moving only once the
  // audio device renders (media/renderers/audio_renderer_impl.cc,
  // GetWallClockTimes()).
  void Play(bool wait_for_audio = false);
  void Pause();
  // Play() was called; time may still be waiting for audio.
  bool playing() const;
  // Playing, and not waiting for audio: media time advances.
  bool moving() const;
  // Continues from the current media time at |rate| (> 0).
  void SetRate(double rate);
  double rate() const;

  // Called by the audio renderer for every rendered buffer: |media_time| is
  // heard at |system_time|. The clock does not run past |max_media_time|,
  // the end of the audio handed to the device, so it stalls on underruns.
  // Once anchored, correct at most 1 ms per buffer: a device route or its
  // initial latency estimate can change without making video skip a gap.
  void UpdateFromAudio(int64_t media_time,
                       int64_t system_time,
                       int64_t max_media_time);

  // Lets the clock run freely, e.g. after audio ended but video continues.
  void ClearMaxTime();

 private:
  int64_t MediaTimeAtLocked(int64_t system_time) const;
  // Playing, and not waiting for audio.
  bool MovingLocked() const;

  mutable std::mutex mutex_;
  int64_t anchor_media_time_ = 0;
  int64_t anchor_system_time_ = 0;
  int64_t max_media_time_ = kInfiniteTime;
  // Media time does not go below this: where Play() started. Audio that is
  // heard later than Play() anchors the clock in the future.
  int64_t min_media_time_ = 0;
  double rate_ = 1.0;
  bool playing_ = false;
  // Play(true) until the first audio update.
  bool waiting_for_audio_ = false;
  bool has_audio_anchor_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_MEDIA_CLOCK_H_
