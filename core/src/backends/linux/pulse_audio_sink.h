#ifndef LYCHEE_BACKENDS_LINUX_PULSE_AUDIO_SINK_H_
#define LYCHEE_BACKENDS_LINUX_PULSE_AUDIO_SINK_H_

#include <pulse/pulseaudio.h>

#include "media/audio_sink.h"

namespace lychee {

// Output through the PulseAudio client API, which PipeWire's pulse server
// also speaks. Everything runs on a pa_threaded_mainloop: the write callback
// pulls from the RenderCallback on that thread with the loop's lock held.
//
// The stream is not pinned to a device, so the server moves it when the
// default sink changes or the current one goes away. When the server itself
// goes away (restarted, crashed), the sink reconnects every second with the
// same format until it is back.
class PulseAudioSink : public AudioSink {
 public:
  PulseAudioSink() = default;
  ~PulseAudioSink() override;

  bool Open(RenderCallback callback, std::string* error) override;
  AudioOutputFormat format() const override { return format_; }
  void Start() override;
  void Stop() override;

 private:
  // All below run on the mainloop thread or with its lock held.
  void Connect();
  void Disconnect();
  void CreateStream();
  void OnFailure(const char* what, int error);
  void ScheduleReconnect();
  int64_t PlayoutTime(pa_stream* stream) const;

  static void OnContextState(pa_context* context, void* user_data);
  static void OnServerInfo(pa_context* context,
                           const pa_server_info* info,
                           void* user_data);
  static void OnStreamState(pa_stream* stream, void* user_data);
  static void OnStreamMoved(pa_stream* stream, void* user_data);
  static void OnWrite(pa_stream* stream, size_t bytes, void* user_data);
  static void OnReconnectTimer(pa_mainloop_api* api,
                               pa_time_event* event,
                               const struct timeval* tv,
                               void* user_data);

  RenderCallback callback_;
  AudioOutputFormat format_;

  pa_threaded_mainloop* mainloop_ = nullptr;
  pa_context* context_ = nullptr;
  pa_stream* stream_ = nullptr;
  pa_time_event* reconnect_timer_ = nullptr;
  bool started_ = false;
  // Open() waits until the first connection is ready or has failed.
  bool opening_ = false;
  bool ready_ = false;
  bool failed_ = false;
  std::string error_;
  // Logged once per outage, not on every retry.
  bool reported_outage_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_LINUX_PULSE_AUDIO_SINK_H_
