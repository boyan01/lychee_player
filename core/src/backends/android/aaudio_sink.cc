#define LOG_TAG "aaudio"

#include "backends/android/aaudio_sink.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <utility>

#include "base/log.h"
#include "base/thread_name.h"
#include "base/time.h"

namespace lychee {

namespace {

constexpr int kChannels = 2;
constexpr auto kRetryInterval = std::chrono::milliseconds(500);

}  // namespace

AAudioSink::~AAudioSink() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closing_ = true;
  }
  condition_.notify_one();
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool AAudioSink::Open(RenderCallback callback, std::string* error) {
  callback_ = std::move(callback);
  AAudioStream* stream = OpenStream(error);
  if (!stream) {
    return false;
  }
  format_ = {AAudioStream_getSampleRate(stream), kChannels};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_ = stream;
  }
  thread_ = std::thread(&AAudioSink::Run, this);
  return true;
}

AAudioStream* AAudioSink::OpenStream(std::string* error) {
  AAudioStreamBuilder* builder = nullptr;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    *error = std::string("AAudio_createStreamBuilder: ") +
             AAudio_convertResultToText(result);
    return nullptr;
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
  AAudioStream* stream = nullptr;
  result = AAudioStreamBuilder_openStream(builder, &stream);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK) {
    *error = std::string("AAudioStreamBuilder_openStream: ") +
             AAudio_convertResultToText(result);
    return nullptr;
  }
  LOGI("opened: %d Hz, %d ch, buffer %d frames",
       AAudioStream_getSampleRate(stream), kChannels,
       AAudioStream_getBufferSizeInFrames(stream));
  return stream;
}

void AAudioSink::CloseStream(std::unique_lock<std::mutex>& lock) {
  disconnected_ = false;
  if (AAudioStream* stream = std::exchange(stream_, nullptr)) {
    // Closing may wait for OnError(). Retire the stream under the lock,
    // then release it before making calls into AAudio.
    lock.unlock();
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    lock.lock();
  }
}

bool AAudioSink::Start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = true;
  }
  // Run() owns recovery, including synchronous start errors.
  condition_.notify_one();
  return true;
}

void AAudioSink::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
  }
  condition_.notify_one();
}

void AAudioSink::Run() {
  SetCurrentThreadName("lychee-aaudio");
  using Clock = std::chrono::steady_clock;
  auto retry_at = Clock::time_point::min();
  bool running = false;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!closing_) {
    if (disconnected_) {
      CloseStream(lock);
      running = false;
      retry_at = Clock::time_point::min();
    } else if (started_ == running) {
      condition_.wait(lock);
    } else if (!started_) {
      // Pause keeps queued audio for a normal resume.
      lock.unlock();
      const auto result = AAudioStream_requestPause(stream_);
      lock.lock();
      running = false;
      if (result != AAUDIO_OK) {
        CloseStream(lock);
      }
    } else if (Clock::now() < retry_at) {
      condition_.wait_until(lock, retry_at);
    } else if (!stream_) {
      std::string error;
      lock.unlock();
      AAudioStream* stream = OpenStream(&error);
      if (!stream) {
        LOGW("reopen failed: %s", error.c_str());
      }
      lock.lock();
      stream_ = stream;
      if (!stream_) {
        retry_at = Clock::now() + kRetryInterval;
      }
    } else {
      start_time_ = NowMicros();
      lock.unlock();
      const auto result = AAudioStream_requestStart(stream_);
      if (result != AAUDIO_OK) {
        LOGW("start failed: %s", AAudio_convertResultToText(result));
      }
      lock.lock();
      running = result == AAUDIO_OK;
      if (!running) {
        CloseStream(lock);
        retry_at = Clock::now() + kRetryInterval;
      }
    }
  }
  CloseStream(lock);
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
void AAudioSink::OnError(AAudioStream* stream,
                         void* user_data,
                         aaudio_result_t error) {
  auto* self = static_cast<AAudioSink*>(user_data);
  LOGW("stream error: %s", AAudio_convertResultToText(error));
  if (error != AAUDIO_ERROR_DISCONNECTED) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->closing_ || self->stream_ != stream) {
      return;
    }
    self->disconnected_ = true;
  }
  self->condition_.notify_one();
}

}  // namespace lychee
