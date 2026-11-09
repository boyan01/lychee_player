#ifndef LYCHEE_PIPELINE_AUDIO_TAP_H_
#define LYCHEE_PIPELINE_AUDIO_TAP_H_

#include <cstdint>
#include <vector>

#include "base/time.h"

namespace lychee {

// The last seconds of what the audio device plays, mixed down to mono and
// placed on the NowMicros() timeline by when each frame is heard, for
// visualizers to read.
class AudioTap {
 public:
  // Allocates room for |capacity| frames; until then Write() and Read()
  // keep and return nothing.
  void Initialize(int sample_rate, int capacity);
  bool initialized() const { return !ring_.empty(); }

  // |frames| frames of |channels| interleaved samples, heard from
  // |playout_time| on: |written| of them audio, the rest the silence the
  // device plays after it.
  void Write(const float* samples,
             int channels,
             int written,
             int frames,
             int64_t playout_time);
  // The |frames| frames heard up to |time|, into |out|: silence where
  // nothing was written.
  void Read(int64_t time, float* out, int frames) const;

 private:
  int sample_rate_ = 0;
  std::vector<float> ring_;
  // Frames written so far, counted from the first; ring_[i % size] holds
  // frame i for i from |begin_| up to |end_|.
  int64_t begin_ = 0;
  int64_t end_ = 0;
  // Frame |anchor_frame_| is heard at |anchor_time_|, and those after it
  // one frame duration apart. kNoTimestamp before the first write.
  int64_t anchor_time_ = kNoTimestamp;
  int64_t anchor_frame_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_AUDIO_TAP_H_
