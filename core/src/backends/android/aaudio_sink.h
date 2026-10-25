#ifndef LYCHEE_BACKENDS_ANDROID_AAUDIO_SINK_H_
#define LYCHEE_BACKENDS_ANDROID_AAUDIO_SINK_H_

#include <aaudio/AAudio.h>

#include <mutex>
#include <thread>

#include "media/audio_sink.h"

namespace lychee {

// Output through AAudio (Android 8+). The playout time of each buffer comes
// from the stream's presentation timestamps, so A/V sync includes Bluetooth
// latency. When the device goes away (headphones unplugged) the stream is
// reopened on the new default device with the same format.
class AAudioSink : public AudioSink {
 public:
  AAudioSink() = default;
  ~AAudioSink() override;

  bool Open(RenderCallback callback, std::string* error) override;
  AudioOutputFormat format() const override { return format_; }
  void Start() override;
  void Stop() override;

 private:
  bool OpenStreamLocked(std::string* error);
  void CloseStreamLocked();
  void Reopen();
  static aaudio_data_callback_result_t OnData(AAudioStream* stream,
                                              void* user_data,
                                              void* audio_data,
                                              int32_t frames);
  static void OnError(AAudioStream* stream,
                      void* user_data,
                      aaudio_result_t error);

  RenderCallback callback_;
  AudioOutputFormat format_;

  std::mutex mutex_;
  AAudioStream* stream_ = nullptr;
  bool started_ = false;
  bool closing_ = false;
  // Reopens the stream: the error callback must not close it itself.
  std::thread reopen_thread_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_AAUDIO_SINK_H_
