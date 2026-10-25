#define LOG_TAG "pulse_audio"

#include "backends/linux/pulse_audio_sink.h"

#include <errno.h>

#include <algorithm>
#include <cstring>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

constexpr int kChannels = 2;
constexpr size_t kFrameBytes = sizeof(float) * kChannels;
// Buffered in the server, including the device's own latency. Larger than a
// desktop audio app would pick: A/V sync accounts for it, and it rides out
// scheduling hiccups in VMs and on loaded machines.
constexpr pa_usec_t kTargetLatency = 60 * PA_USEC_PER_MSEC;
constexpr pa_usec_t kReconnectDelay = PA_USEC_PER_SEC;
// How long Open() waits for the server before audio is disabled.
constexpr pa_usec_t kOpenTimeout = 3 * PA_USEC_PER_SEC;

pa_proplist* NewProperties() {
  pa_proplist* properties = pa_proplist_new();
  pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME,
                   program_invocation_short_name);
  pa_proplist_sets(properties, PA_PROP_MEDIA_ROLE, "video");
  return properties;
}

const char* DeviceName(pa_stream* stream) {
  const char* name = pa_stream_get_device_name(stream);
  return name ? name : "?";
}

}  // namespace

PulseAudioSink::~PulseAudioSink() {
  if (!mainloop_) {
    return;
  }
  pa_threaded_mainloop_lock(mainloop_);
  if (reconnect_timer_) {
    pa_threaded_mainloop_get_api(mainloop_)->time_free(reconnect_timer_);
    reconnect_timer_ = nullptr;
  }
  Disconnect();
  pa_threaded_mainloop_unlock(mainloop_);
  // Joins the mainloop thread: no callback runs after this.
  pa_threaded_mainloop_stop(mainloop_);
  pa_threaded_mainloop_free(mainloop_);
}

bool PulseAudioSink::Open(RenderCallback callback, std::string* error) {
  callback_ = std::move(callback);
  mainloop_ = pa_threaded_mainloop_new();
  if (!mainloop_) {
    *error = "pa_threaded_mainloop_new failed";
    return false;
  }
  pa_threaded_mainloop_set_name(mainloop_, "lychee-pulse");
  if (pa_threaded_mainloop_start(mainloop_) < 0) {
    *error = "pa_threaded_mainloop_start failed";
    pa_threaded_mainloop_free(mainloop_);
    mainloop_ = nullptr;
    return false;
  }

  pa_threaded_mainloop_lock(mainloop_);
  opening_ = true;
  // Reuses the reconnect timer slot: no reconnect is scheduled while
  // opening, failures are reported to the caller instead.
  pa_mainloop_api* api = pa_threaded_mainloop_get_api(mainloop_);
  struct timeval deadline;
  pa_timeval_add(pa_gettimeofday(&deadline), kOpenTimeout);
  reconnect_timer_ = api->time_new(api, &deadline, &OnReconnectTimer, this);
  Connect();
  while (!ready_ && !failed_) {
    pa_threaded_mainloop_wait(mainloop_);
  }
  opening_ = false;
  if (reconnect_timer_) {
    api->time_free(reconnect_timer_);
    reconnect_timer_ = nullptr;
  }
  const bool ok = ready_;
  if (!ok) {
    *error = error_;
    Disconnect();
  }
  pa_threaded_mainloop_unlock(mainloop_);

  if (!ok) {
    pa_threaded_mainloop_stop(mainloop_);
    pa_threaded_mainloop_free(mainloop_);
    mainloop_ = nullptr;
  }
  return ok;
}

void PulseAudioSink::Start() {
  if (!mainloop_) {
    return;
  }
  pa_threaded_mainloop_lock(mainloop_);
  started_ = true;
  if (stream_ && pa_stream_get_state(stream_) == PA_STREAM_READY) {
    if (pa_operation* operation = pa_stream_cork(stream_, 0, nullptr, nullptr)) {
      pa_operation_unref(operation);
    }
  }
  pa_threaded_mainloop_unlock(mainloop_);
}

void PulseAudioSink::Stop() {
  if (!mainloop_) {
    return;
  }
  pa_threaded_mainloop_lock(mainloop_);
  started_ = false;
  if (stream_ && pa_stream_get_state(stream_) == PA_STREAM_READY) {
    // Corking pauses without dropping what the server has buffered, so
    // Start() resumes where playback stopped.
    if (pa_operation* operation = pa_stream_cork(stream_, 1, nullptr, nullptr)) {
      pa_operation_unref(operation);
    }
  }
  pa_threaded_mainloop_unlock(mainloop_);
}

void PulseAudioSink::Connect() {
  pa_proplist* properties = NewProperties();
  context_ = pa_context_new_with_proplist(
      pa_threaded_mainloop_get_api(mainloop_), nullptr, properties);
  pa_proplist_free(properties);
  if (!context_) {
    OnFailure("pa_context_new", PA_ERR_INTERNAL);
    return;
  }
  pa_context_set_state_callback(context_, &OnContextState, this);
  // No autospawn: a desktop session already runs its sound server.
  if (pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) <
      0) {
    OnFailure("pa_context_connect", pa_context_errno(context_));
  }
}

void PulseAudioSink::Disconnect() {
  if (stream_) {
    pa_stream_set_state_callback(stream_, nullptr, nullptr);
    pa_stream_set_write_callback(stream_, nullptr, nullptr);
    pa_stream_set_moved_callback(stream_, nullptr, nullptr);
    pa_stream_disconnect(stream_);
    pa_stream_unref(stream_);
    stream_ = nullptr;
  }
  if (context_) {
    pa_context_set_state_callback(context_, nullptr, nullptr);
    pa_context_disconnect(context_);
    pa_context_unref(context_);
    context_ = nullptr;
  }
}

void PulseAudioSink::CreateStream() {
  const pa_sample_spec spec = {PA_SAMPLE_FLOAT32NE,
                               static_cast<uint32_t>(format_.sample_rate),
                               kChannels};
  pa_channel_map map;
  pa_channel_map_init_stereo(&map);
  pa_proplist* properties = NewProperties();
  stream_ = pa_stream_new_with_proplist(context_, "Playback", &spec, &map,
                                        properties);
  pa_proplist_free(properties);
  if (!stream_) {
    OnFailure("pa_stream_new", pa_context_errno(context_));
    return;
  }
  pa_stream_set_state_callback(stream_, &OnStreamState, this);
  pa_stream_set_write_callback(stream_, &OnWrite, this);
  pa_stream_set_moved_callback(stream_, &OnStreamMoved, this);

  pa_buffer_attr attributes;
  attributes.maxlength = static_cast<uint32_t>(-1);
  attributes.tlength = static_cast<uint32_t>(pa_usec_to_bytes(kTargetLatency, &spec));
  attributes.prebuf = static_cast<uint32_t>(-1);
  attributes.minreq = static_cast<uint32_t>(-1);
  attributes.fragsize = static_cast<uint32_t>(-1);
  // No device: the server picks the default sink, and moves the stream when
  // the default changes or the sink disappears.
  int flags = PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE |
              PA_STREAM_ADJUST_LATENCY;
  if (!started_) {
    flags |= PA_STREAM_START_CORKED;
  }
  if (pa_stream_connect_playback(stream_, nullptr, &attributes,
                                 static_cast<pa_stream_flags_t>(flags), nullptr,
                                 nullptr) < 0) {
    OnFailure("pa_stream_connect_playback", pa_context_errno(context_));
  }
}

void PulseAudioSink::OnFailure(const char* what, int error) {
  if (opening_) {
    failed_ = true;
    error_ = std::string(what) + ": " + pa_strerror(error);
    pa_threaded_mainloop_signal(mainloop_, 0);
    return;
  }
  if (!reported_outage_) {
    LOGW("%s: %s; reconnecting", what, pa_strerror(error));
    reported_outage_ = true;
  }
  ScheduleReconnect();
}

void PulseAudioSink::ScheduleReconnect() {
  if (reconnect_timer_) {
    return;
  }
  // The failed context is torn down from the timer, not from inside its
  // own callback.
  pa_mainloop_api* api = pa_threaded_mainloop_get_api(mainloop_);
  struct timeval when;
  pa_timeval_add(pa_gettimeofday(&when), kReconnectDelay);
  reconnect_timer_ = api->time_new(api, &when, &OnReconnectTimer, this);
}

int64_t PulseAudioSink::PlayoutTime(pa_stream* stream) const {
  // The time until the last sample written so far is heard; the next one
  // follows it.
  pa_usec_t latency = 0;
  int negative = 0;
  if (pa_stream_get_latency(stream, &latency, &negative) == 0) {
    return NowMicros() + (negative ? 0 : static_cast<int64_t>(latency));
  }
  // No timing information yet: assume a full buffer.
  const pa_buffer_attr* attributes = pa_stream_get_buffer_attr(stream);
  const pa_usec_t buffered =
      attributes ? pa_bytes_to_usec(attributes->tlength,
                                    pa_stream_get_sample_spec(stream))
                 : kTargetLatency;
  return NowMicros() + static_cast<int64_t>(buffered);
}

// static
void PulseAudioSink::OnContextState(pa_context* context, void* user_data) {
  auto* self = static_cast<PulseAudioSink*>(user_data);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
      if (self->format_.sample_rate > 0) {
        // Reconnecting: keep the format the decoders convert to; the server
        // resamples if its rate changed.
        self->CreateStream();
      } else if (pa_operation* operation = pa_context_get_server_info(
                     context, &OnServerInfo, self)) {
        pa_operation_unref(operation);
      } else {
        self->OnFailure("pa_context_get_server_info", pa_context_errno(context));
      }
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      self->OnFailure("context", pa_context_errno(context));
      break;
    default:
      break;
  }
}

// static
void PulseAudioSink::OnServerInfo(pa_context*,
                                  const pa_server_info* info,
                                  void* user_data) {
  auto* self = static_cast<PulseAudioSink*>(user_data);
  // Render at the server's rate so it does not resample.
  uint32_t rate = info ? info->sample_spec.rate : 0;
  if (rate < 8000 || rate > 384000) {
    rate = 48000;
  }
  self->format_ = {static_cast<int>(rate), kChannels};
  self->CreateStream();
}

// static
void PulseAudioSink::OnStreamState(pa_stream* stream, void* user_data) {
  auto* self = static_cast<PulseAudioSink*>(user_data);
  switch (pa_stream_get_state(stream)) {
    case PA_STREAM_READY: {
      const pa_buffer_attr* attributes = pa_stream_get_buffer_attr(stream);
      LOGI("%s: %d Hz, %d ch, device %s, buffer %u bytes",
           self->reported_outage_ ? "reconnected" : "opened",
           self->format_.sample_rate, kChannels,
           DeviceName(stream), attributes ? attributes->tlength : 0);
      self->reported_outage_ = false;
      if (self->opening_) {
        self->ready_ = true;
        pa_threaded_mainloop_signal(self->mainloop_, 0);
      }
      break;
    }
    case PA_STREAM_FAILED:
    case PA_STREAM_TERMINATED:
      self->OnFailure("stream", pa_context_errno(self->context_));
      break;
    default:
      break;
  }
}

// static
void PulseAudioSink::OnStreamMoved(pa_stream* stream, void*) {
  // The server moved the stream (new default sink, or the old one went
  // away). Latency changes are picked up by the timing updates.
  LOGI("moved to %s", DeviceName(stream));
}

// static
void PulseAudioSink::OnWrite(pa_stream* stream, size_t bytes, void* user_data) {
  auto* self = static_cast<PulseAudioSink*>(user_data);
  while (bytes >= kFrameBytes) {
    void* data = nullptr;
    size_t size = bytes;
    if (pa_stream_begin_write(stream, &data, &size) < 0 || !data) {
      return;
    }
    size = std::min(size, bytes) / kFrameBytes * kFrameBytes;
    if (size == 0) {
      pa_stream_cancel_write(stream);
      return;
    }
    const int frames = static_cast<int>(size / kFrameBytes);
    auto* destination = static_cast<float*>(data);
    const int written =
        self->callback_(destination, frames, self->PlayoutTime(stream));
    if (written < frames) {
      memset(destination + written * kChannels, 0,
             static_cast<size_t>(frames - written) * kFrameBytes);
    }
    pa_stream_write(stream, data, size, nullptr, 0, PA_SEEK_RELATIVE);
    bytes -= size;
  }
}

// static
void PulseAudioSink::OnReconnectTimer(pa_mainloop_api* api,
                                      pa_time_event* event,
                                      const struct timeval*,
                                      void* user_data) {
  auto* self = static_cast<PulseAudioSink*>(user_data);
  api->time_free(event);
  self->reconnect_timer_ = nullptr;
  if (self->opening_) {
    self->OnFailure("connect", PA_ERR_TIMEOUT);
    return;
  }
  self->Disconnect();
  self->Connect();
}

}  // namespace lychee
