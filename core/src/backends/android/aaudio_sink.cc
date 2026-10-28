#define LOG_TAG "aaudio"

#include "backends/android/aaudio_sink.h"

#include <cstring>
#include <ctime>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

constexpr int kChannels = 2;

}  // namespace

AAudioSink::~AAudioSink() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  if (reopen_thread_.joinable()) {
    reopen_thread_.join();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  CloseStreamLocked();
}

bool AAudioSink::Open(RenderCallback callback, std::string* error) {
  callback_ = std::move(callback);
  std::lock_guard<std::mutex> lock(mutex_);
  return OpenStreamLocked(error);
}

bool AAudioSink::OpenStreamLocked(std::string* error) {
  AAudioStreamBuilder* builder = nullptr;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    *error = std::string("AAudio_createStreamBuilder: ") +
             AAudio_convertResultToText(result);
    return false;
  }
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(builder, kChannels);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setPerformanceMode(builder,
                                         AAUDIO_PERFORMANCE_MODE_POWER_SAVING);
  // Reopening keeps the format the decoders already convert to; AAudio
  // resamples if the new device differs.
  if (format_.sample_rate > 0) {
    AAudioStreamBuilder_setSampleRate(builder, format_.sample_rate);
  }
  AAudioStreamBuilder_setDataCallback(builder, &OnData, this);
  AAudioStreamBuilder_setErrorCallback(builder, &OnError, this);
  result = AAudioStreamBuilder_openStream(builder, &stream_);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK) {
    *error = std::string("AAudioStreamBuilder_openStream: ") +
             AAudio_convertResultToText(result);
    stream_ = nullptr;
    return false;
  }
  format_ = {AAudioStream_getSampleRate(stream_), kChannels};
  LOGI("opened: %d Hz, %d ch, buffer %d frames", format_.sample_rate,
       kChannels, AAudioStream_getBufferSizeInFrames(stream_));
  return true;
}

void AAudioSink::CloseStreamLocked() {
  if (stream_) {
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }
}

void AAudioSink::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = true;
  if (stream_) {
    start_time_ = NowMicros();
    AAudioStream_requestStart(stream_);
  }
}

void AAudioSink::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
  if (stream_) {
    // Pause, not stop: Start() resumes without flushing.
    AAudioStream_requestPause(stream_);
  }
}

void AAudioSink::Reopen() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closing_) {
    return;
  }
  CloseStreamLocked();
  std::string error;
  if (!OpenStreamLocked(&error)) {
    LOGE("reopen failed: %s", error.c_str());
    return;
  }
  if (started_) {
    start_time_ = NowMicros();
    AAudioStream_requestStart(stream_);
  }
}

// static
aaudio_data_callback_result_t AAudioSink::OnData(AAudioStream* stream,
                                                 void* user_data,
                                                 void* audio_data,
                                                 int32_t frames) {
  auto* self = static_cast<AAudioSink*>(user_data);
  const int rate = AAudioStream_getSampleRate(stream);
  // When the next frame written will be heard: the stream reports when a
  // past frame was presented; frames written since are still queued.
  int64_t presented_frame = 0;
  int64_t presented_nanos = 0;
  int64_t playout_time;
  if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &presented_frame,
                                &presented_nanos) == AAUDIO_OK &&
      presented_nanos / 1000 >= self->start_time_) {
    const int64_t queued =
        AAudioStream_getFramesWritten(stream) - presented_frame;
    playout_time =
        presented_nanos / 1000 + queued * kMicrosPerSecond / rate;
  } else {
    // Not presenting yet, or not since the last start: assume one buffer
    // of latency.
    playout_time = NowMicros() + static_cast<int64_t>(
                                     AAudioStream_getBufferSizeInFrames(stream)) *
                                     kMicrosPerSecond / rate;
  }
  auto* destination = static_cast<float*>(audio_data);
  const int written = self->callback_(destination, frames, playout_time);
  if (written < frames) {
    memset(destination + static_cast<ptrdiff_t>(written) * kChannels, 0,
           static_cast<size_t>(frames - written) * sizeof(float) * kChannels);
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// static
void AAudioSink::OnError(AAudioStream*, void* user_data, aaudio_result_t error) {
  auto* self = static_cast<AAudioSink*>(user_data);
  LOGW("stream error: %s", AAudio_convertResultToText(error));
  if (error != AAUDIO_ERROR_DISCONNECTED) {
    return;
  }
  // Runs on an AAudio thread that must not close the stream.
  std::lock_guard<std::mutex> lock(self->mutex_);
  if (self->closing_) {
    return;
  }
  if (self->reopen_thread_.joinable()) {
    self->reopen_thread_.detach();
  }
  self->reopen_thread_ = std::thread([self] { self->Reopen(); });
}

}  // namespace lychee
