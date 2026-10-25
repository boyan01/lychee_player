#ifndef LYCHEE_BACKENDS_APPLE_CORE_AUDIO_SINK_H_
#define LYCHEE_BACKENDS_APPLE_CORE_AUDIO_SINK_H_

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>

#include <atomic>

#if TARGET_OS_OSX
#include <CoreAudio/CoreAudio.h>
#endif

#include "media/audio_sink.h"

namespace lychee {

// Output through the default output AudioUnit (RemoteIO on iOS). The unit
// follows the default device or route and converts to its sample rate; the
// sink tracks changes only to keep the output latency, and so A/V sync,
// right. On iOS it also restarts the unit after an interruption.
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
  void UpdateOutputLatency();

  AudioUnit unit_ = nullptr;
  RenderCallback callback_;
  AudioOutputFormat format_;
  // Read on the render thread, written on |listener_queue_|.
  std::atomic<int64_t> output_latency_{0};
  std::atomic<bool> started_{false};
#if TARGET_OS_OSX
  dispatch_queue_t listener_queue_ = nullptr;
  AudioObjectPropertyListenerBlock listener_ = nullptr;
#elif TARGET_OS_IOS
  void* session_observer_ = nullptr;
#endif
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_CORE_AUDIO_SINK_H_
