#ifndef LYCHEE_MEDIA_AUDIO_BUFFER_H_
#define LYCHEE_MEDIA_AUDIO_BUFFER_H_

#include <cstdint>
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

}  // namespace lychee

#endif  // LYCHEE_MEDIA_AUDIO_BUFFER_H_
