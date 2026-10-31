#ifndef LYCHEE_MEDIA_AUDIO_BUFFER_H_
#define LYCHEE_MEDIA_AUDIO_BUFFER_H_

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "base/time.h"

namespace lychee {

// Decoded audio: interleaved float samples in the sink's format.
struct AudioBuffer {
  int64_t pts = kNoTimestamp;
  int sample_rate = 0;
  int channels = 0;
  std::vector<float> samples;

  int frames() const {
    return channels > 0 ? static_cast<int>(samples.size()) / channels : 0;
  }
  int64_t duration() const {
    return sample_rate > 0 ? frames() * kMicrosPerSecond / sample_rate : 0;
  }
};

// For decoders that count output timestamps in frames from an input
// timestamp. The input and the count differ by a constant (the decoder's
// delay against the container's timestamps), which is kept. When the input
// jumps from one packet to the next (a sender skipping silence, a live
// timeline moving audio), the count follows it; a slow drift is left alone,
// as stepping back to it would repeat audio.
class AudioTimestampFollower {
 public:
  // The shift to add to the counted timestamps, which stand at |counted|
  // for an input at |pts|.
  int64_t Follow(int64_t pts, int64_t counted) {
    if (pts == kNoTimestamp || counted == kNoTimestamp) {
      return 0;
    }
    const int64_t drift = pts - counted;
    if (drift_ == kNoTimestamp) {
      drift_ = last_ = drift;
      return 0;
    }
    const int64_t jump = drift - last_;
    last_ = drift;
    if (std::abs(jump) <= kTolerance) {
      return 0;
    }
    last_ = drift_;
    return drift - drift_;
  }
  void Reset() { drift_ = kNoTimestamp; }

 private:
  // Below a live timeline's smallest move, above timestamp rounding.
  static constexpr int64_t kTolerance = 5 * kMicrosPerMilli;
  int64_t drift_ = kNoTimestamp;
  int64_t last_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_AUDIO_BUFFER_H_
