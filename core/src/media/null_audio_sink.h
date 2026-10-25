#ifndef LYCHEE_MEDIA_NULL_AUDIO_SINK_H_
#define LYCHEE_MEDIA_NULL_AUDIO_SINK_H_

#include <cstdint>
#include <vector>

#include "media/audio_sink.h"

namespace lychee {

// A silent audio device for virtual time (base/time.h). It has no thread:
// RenderAll() pulls every started sink up to the new time, on the caller's
// thread, as a device would have played it in that time.
class NullAudioSink : public AudioSink {
 public:
  NullAudioSink();
  ~NullAudioSink() override;

  // Pulls each started sink until |now| plus its latency is covered.
  static void RenderAll(int64_t now);

  bool Open(RenderCallback callback, std::string* error) override;
  AudioOutputFormat format() const override;
  void Start() override;
  void Stop() override;

 private:
  void RenderUntil(int64_t now);

  RenderCallback callback_;
  AudioOutputFormat format_;
  // Guarded by the sink list's mutex while started.
  int64_t start_time_ = 0;
  int64_t frames_rendered_ = 0;
  std::vector<float> buffer_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_NULL_AUDIO_SINK_H_
