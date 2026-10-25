#ifndef LYCHEE_BACKENDS_WINDOWS_WASAPI_SINK_H_
#define LYCHEE_BACKENDS_WINDOWS_WASAPI_SINK_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "media/audio_sink.h"

namespace lychee {

// Output through shared-mode, event-driven WASAPI on the default render
// endpoint. The engine converts the stereo float stream to the device's mix
// format. The playout time of each buffer comes from IAudioClock, so A/V sync
// includes the device's latency. When the default device changes or the
// current one goes away, the stream is reopened on the new default device
// with the same format.
//
// All COM objects live on the sink's own render thread; Start() and Stop()
// hand it a command and wait until it is applied, so after Stop() returns no
// callback runs.
class WasapiSink : public AudioSink {
 public:
  WasapiSink() = default;
  ~WasapiSink() override;

  bool Open(RenderCallback callback, std::string* error) override;
  AudioOutputFormat format() const override { return format_; }
  void Start() override;
  void Stop() override;

 private:
  class Stream;
  class DeviceListener;

  void Run(std::string* error);
  void SetStarted(bool started);

  RenderCallback callback_;
  AudioOutputFormat format_;
  std::thread thread_;
  // An auto-reset event (HANDLE) that wakes the render thread for commands,
  // device changes and shutdown.
  void* wake_event_ = nullptr;
  // Set from the device listener's thread.
  std::atomic<bool> device_changed_{false};

  std::mutex mutex_;
  std::condition_variable condition_;
  // Guarded by |mutex_|.
  bool open_finished_ = false;
  // Set when the render thread exits or must exit.
  bool quit_ = false;
  bool want_started_ = false;
  uint64_t commands_sent_ = 0;
  uint64_t commands_applied_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_WINDOWS_WASAPI_SINK_H_
