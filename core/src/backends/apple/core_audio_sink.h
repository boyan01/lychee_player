#ifndef LYCHEE_BACKENDS_APPLE_CORE_AUDIO_SINK_H_
#define LYCHEE_BACKENDS_APPLE_CORE_AUDIO_SINK_H_

#include <AudioToolbox/AudioToolbox.h>

#include "media/audio_sink.h"

namespace lychee {

// Output through the default output AudioUnit.
class CoreAudioSink : public AudioSink {
 public:
  CoreAudioSink();
  ~CoreAudioSink() override;

  bool Open(RenderCallback callback, std::string* error) override;
  AudioOutputFormat format() const override { return format_; }
  void Start() override;
  void Stop() override;

 private:
  static OSStatus OnRender(void* user_data,
                           AudioUnitRenderActionFlags* flags,
                           const AudioTimeStamp* timestamp,
                           UInt32 bus,
                           UInt32 frame_count,
                           AudioBufferList* buffers);
  int64_t QueryOutputLatency() const;

  AudioUnit unit_ = nullptr;
  RenderCallback callback_;
  AudioOutputFormat format_;
  int64_t output_latency_ = 0;
  bool started_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_CORE_AUDIO_SINK_H_
