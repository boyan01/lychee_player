#ifndef LYCHEE_MEDIA_AUDIO_SINK_H_
#define LYCHEE_MEDIA_AUDIO_SINK_H_

#include <cstdint>
#include <functional>
#include <string>

#include "media/audio_decoder.h"

namespace lychee {

// Platform audio output. Pulls interleaved float samples.
class AudioSink {
 public:
  // Runs on the real-time audio thread. Writes up to |frames| frames into
  // |destination| and returns how many were written; the sink fills the rest
  // with silence. |playout_time| is when the first frame will be heard, in
  // the NowMicros() timebase.
  using RenderCallback =
      std::function<int(float* destination, int frames, int64_t playout_time)>;

  virtual ~AudioSink() = default;

  virtual bool Open(RenderCallback callback, std::string* error) = 0;
  // Valid after Open().
  virtual AudioOutputFormat format() const = 0;
  // False if the device did not start (iOS refuses a session in the
  // background); the caller retries. Sinks that recover by themselves
  // (reopening a lost device) return true.
  virtual bool Start() = 0;
  virtual void Stop() = 0;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_AUDIO_SINK_H_
