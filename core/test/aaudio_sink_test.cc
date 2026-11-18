// Run on a Clang host with tool/aaudio_sink_check.sh ANDROID_NDK.
#include "backends/android/aaudio_sink.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "test.h"

struct AAudioStreamBuilderStruct {
  AAudioStream_dataCallback data = nullptr;
  AAudioStream_errorCallback error = nullptr;
  void* user = nullptr;
};

struct AAudioStreamStruct {
  AAudioStreamBuilderStruct callbacks;
};

namespace {

using namespace std::chrono_literals;

std::atomic<AAudioStream*> g_stream{nullptr};
std::atomic<int> g_open_attempts{0};
std::atomic<int> g_start_attempts{0};
std::atomic<int> g_closes{0};
std::atomic<int> g_pulls{0};
std::atomic<int> g_open_failures{0};
std::atomic<int> g_start_failures{0};

void Reset() {
  g_open_attempts = 0;
  g_start_attempts = 0;
  g_closes = 0;
  g_pulls = 0;
  g_open_failures = 0;
  g_start_failures = 0;
}

bool WaitFor(const std::atomic<int>& count, int expected) {
  const auto until = std::chrono::steady_clock::now() + 2s;
  while (count < expected && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(1ms);
  }
  return count >= expected;
}

int Render(float* out, int frames, int64_t) {
  for (int i = 0; i < 2 * frames; ++i) {
    out[i] = 0.5f;
  }
  return frames;
}

void Disconnect() {
  auto* stream = g_stream.load();
  if (!stream) {
    EXPECT_TRUE(stream);
    return;
  }
  const auto callbacks = stream->callbacks;
  callbacks.error(stream, callbacks.user, AAUDIO_ERROR_DISCONNECTED);
}

}  // namespace

extern "C" {

const char* AAudio_convertResultToText(aaudio_result_t) {
  return "injected";
}
aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder** out) {
  *out = new AAudioStreamBuilder;
  return AAUDIO_OK;
}
aaudio_result_t AAudioStreamBuilder_delete(AAudioStreamBuilder* builder) {
  delete builder;
  return AAUDIO_OK;
}
void AAudioStreamBuilder_setDirection(AAudioStreamBuilder*,
                                      aaudio_direction_t) {}
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder*, aaudio_format_t) {}
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder*, int32_t) {}
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder*,
                                        aaudio_sharing_mode_t) {}
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder*,
                                            aaudio_performance_mode_t) {}
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder*, int32_t) {}
void AAudioStreamBuilder_setDataCallback(AAudioStreamBuilder* builder,
                                         AAudioStream_dataCallback callback,
                                         void* user) {
  builder->data = callback;
  builder->user = user;
}
void AAudioStreamBuilder_setErrorCallback(AAudioStreamBuilder* builder,
                                          AAudioStream_errorCallback callback,
                                          void* user) {
  builder->error = callback;
  builder->user = user;
}
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder,
                                               AAudioStream** out) {
  const int attempt = ++g_open_attempts;
  if (attempt > 1 && attempt - 1 <= g_open_failures) {
    return AAUDIO_ERROR_NO_SERVICE;
  }
  *out = new AAudioStream{*builder};
  g_stream = *out;
  return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestStop(AAudioStream*) {
  return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestPause(AAudioStream*) {
  return AAUDIO_OK;
}
aaudio_result_t AAudioStream_close(AAudioStream* stream) {
  g_stream = nullptr;
  delete stream;
  ++g_closes;
  return AAUDIO_OK;
}
aaudio_result_t AAudioStream_requestStart(AAudioStream* stream) {
  if (++g_start_attempts <= g_start_failures) {
    return AAUDIO_ERROR_INVALID_STATE;
  }
  float samples[16] = {};
  const auto& callbacks = stream->callbacks;
  callbacks.data(stream, callbacks.user, samples, 8);
  if (samples[15] == 0.5f) {
    ++g_pulls;
  }
  return AAUDIO_OK;
}
int32_t AAudioStream_getSampleRate(AAudioStream*) {
  return 48000;
}
int32_t AAudioStream_getBufferSizeInFrames(AAudioStream*) {
  return 480;
}
int64_t AAudioStream_getFramesWritten(AAudioStream*) {
  return 0;
}
aaudio_result_t AAudioStream_getTimestamp(AAudioStream*,
                                          clockid_t,
                                          int64_t*,
                                          int64_t*) {
  return AAUDIO_ERROR_INVALID_STATE;
}

}  // extern "C"

// NOLINTBEGIN(bugprone-throwing-static-initialization)

TEST(AAudioSink, RetriesOpenWhilePlaying) {
  Reset();
  g_open_failures = 1;
  lychee::AAudioSink sink;
  std::string error;
  EXPECT_TRUE(sink.Open(Render, &error));
  EXPECT_TRUE(sink.Start());
  if (!WaitFor(g_pulls, 1)) {
    EXPECT_TRUE(false);
    return;
  }
  Disconnect();
  EXPECT_TRUE(WaitFor(g_pulls, 2));
  EXPECT_EQ(g_open_attempts.load(), 3);
  EXPECT_EQ(g_start_attempts.load(), 2);
}

TEST(AAudioSink, RetriesSynchronousStartFailure) {
  Reset();
  g_start_failures = 1;
  lychee::AAudioSink sink;
  std::string error;
  EXPECT_TRUE(sink.Open(Render, &error));
  EXPECT_TRUE(sink.Start());
  EXPECT_TRUE(WaitFor(g_pulls, 1));
  EXPECT_EQ(g_start_attempts.load(), 2);
}

TEST(AAudioSink, PausingCancelsRecoveryUntilPlaybackResumes) {
  Reset();
  g_open_failures = 1;
  {
    lychee::AAudioSink sink;
    std::string error;
    EXPECT_TRUE(sink.Open(Render, &error));
    EXPECT_TRUE(sink.Start());
    if (!WaitFor(g_pulls, 1)) {
      EXPECT_TRUE(false);
      return;
    }
    Disconnect();
    EXPECT_TRUE(WaitFor(g_open_attempts, 2));
    sink.Stop();
    std::this_thread::sleep_for(750ms);
    EXPECT_EQ(g_open_attempts.load(), 2);
    EXPECT_EQ(g_pulls.load(), 1);
    EXPECT_TRUE(sink.Start());
    EXPECT_TRUE(WaitFor(g_pulls, 2));
  }
  EXPECT_EQ(g_closes.load(), 2);
  EXPECT_TRUE(g_stream == nullptr);
}

// NOLINTEND(bugprone-throwing-static-initialization)
