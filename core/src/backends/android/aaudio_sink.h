#ifndef LYCHEE_BACKENDS_ANDROID_AAUDIO_SINK_H_
#define LYCHEE_BACKENDS_ANDROID_AAUDIO_SINK_H_

#include <aaudio/AAudio.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
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
  bool Start() override;
  void Stop() override;

 private:
  AAudioStream* OpenStream(std::string* error);
  void CloseStream(std::unique_lock<std::mutex>& lock);
  void Run();
  static aaudio_data_callback_result_t OnData(AAudioStream* stream,
                                              void* user_data,
                                              void* audio_data,
                                              int32_t frames);
  static void OnError(AAudioStream* stream,
                      void* user_data,
                      aaudio_result_t error);

  RenderCallback callback_;
  AudioOutputFormat format_;

  // When the stream was last started. A resumed stream reports the
  // timestamp it had before the pause until it presents again.
  std::atomic<int64_t> start_time_{0};

  std::mutex mutex_;
  std::condition_variable condition_;
  AAudioStream* stream_ = nullptr;
  bool started_ = false;
  bool disconnected_ = false;
  bool closing_ = false;
  // Owns the stream after Open() and retries failures while playing.
  std::thread thread_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_AAUDIO_SINK_H_
