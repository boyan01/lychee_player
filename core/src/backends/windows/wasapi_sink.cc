#define LOG_TAG "wasapi"

#include "backends/windows/wasapi_sink.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "base/log.h"
#include "base/thread_name.h"
#include "base/time.h"

namespace lychee {

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kChannels = 2;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out so no GUID library is needed.
constexpr GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// Shared-mode buffer, in 100 ns units. Longer than the engine period (10 ms)
// so a late wakeup does not glitch; its latency is part of the playout time.
constexpr REFERENCE_TIME kBufferDuration = 40LL * 10000;
// The render thread also wakes without an event, to notice a device that
// went away silently and to retry opening one.
constexpr DWORD kPollMillis = 200;
constexpr int64_t kReopenInterval = kMicrosPerSecond;

// QueryPerformanceCounter in microseconds. IAudioClock reports times in this
// clock (in 100 ns units); NowMicros() is steady_clock, which MSVC also
// builds on it, but only differences are taken so the epochs need not match.
int64_t QpcNowMicros() {
  static const int64_t frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
  }();
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart / frequency * kMicrosPerSecond +
         counter.QuadPart % frequency * kMicrosPerSecond / frequency;
}

unsigned long HResultCode(HRESULT hr) {
  return static_cast<unsigned long>(hr);
}

}  // namespace

// One opened IAudioClient on the default device. Render thread only.
class WasapiSink::Stream {
 public:
  ~Stream() {
    if (client_) {
      client_->Stop();
    }
  }

  // |sample_rate| 0 takes the device's mix rate.
  static std::unique_ptr<Stream> Create(IMMDeviceEnumerator* enumerator,
                                        int sample_rate,
                                        HANDLE event,
                                        std::string* error) {
    auto stream = std::unique_ptr<Stream>(new Stream());
    HRESULT hr = stream->Open(enumerator, sample_rate, event, error);
    if (FAILED(hr)) {
      char code[16];
      snprintf(code, sizeof(code), " 0x%08lx", HResultCode(hr));
      *error += code;
      return nullptr;
    }
    return stream;
  }

  int sample_rate() const { return sample_rate_; }

  HRESULT Start(const RenderCallback& callback) {
    // Starting on a full buffer avoids an initial glitch. After a pause the
    // buffer still holds what was not played.
    Fill(callback);
    return client_->Start();
  }

  // A pause: the queued audio stays and plays after Start().
  HRESULT Stop() { return client_->Stop(); }

  // Fills the free part of the buffer.
  HRESULT Fill(const RenderCallback& callback) {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
      return hr;
    }
    const UINT32 frames = buffer_frames_ - padding;
    if (frames == 0) {
      return S_OK;
    }
    BYTE* data = nullptr;
    hr = render_->GetBuffer(frames, &data);
    if (FAILED(hr)) {
      return hr;
    }
    auto* destination = reinterpret_cast<float*>(data);
    const int count = static_cast<int>(frames);
    const int written =
        std::clamp(callback(destination, count, PlayoutTime(padding)), 0, count);
    if (written < count) {
      memset(destination + static_cast<ptrdiff_t>(written) * kChannels, 0,
             static_cast<size_t>(count - written) * sizeof(float) * kChannels);
    }
    hr = render_->ReleaseBuffer(frames, 0);
    frames_written_ += frames;
    return hr;
  }

 private:
  Stream() = default;

  HRESULT Open(IMMDeviceEnumerator* enumerator,
               int sample_rate,
               HANDLE event,
               std::string* error) {
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                     device.GetAddressOf());
    if (FAILED(hr)) {
      *error = "no default audio output device";
      return hr;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client_.GetAddressOf()));
    if (FAILED(hr)) {
      *error = "IMMDevice::Activate";
      return hr;
    }
    if (sample_rate <= 0) {
      WAVEFORMATEX* mix = nullptr;
      hr = client_->GetMixFormat(&mix);
      if (FAILED(hr)) {
        *error = "IAudioClient::GetMixFormat";
        return hr;
      }
      sample_rate = static_cast<int>(mix->nSamplesPerSec);
      CoTaskMemFree(mix);
    }
    sample_rate_ = sample_rate;

    WAVEFORMATEXTENSIBLE format = {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = kChannels;
    format.Format.nSamplesPerSec = static_cast<DWORD>(sample_rate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = kChannels * sizeof(float);
    format.Format.nAvgBytesPerSec =
        format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = kSubtypeFloat;
    // The engine converts channels and, when reopened on a device with
    // another mix rate, the sample rate: the decoders keep their format.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration,
                             0, &format.Format, nullptr);
    if (FAILED(hr)) {
      *error = "IAudioClient::Initialize";
      return hr;
    }
    if (FAILED(hr = client_->SetEventHandle(event)) ||
        FAILED(hr = client_->GetBufferSize(&buffer_frames_)) ||
        FAILED(hr = client_->GetService(
                   __uuidof(IAudioRenderClient),
                   reinterpret_cast<void**>(render_.GetAddressOf()))) ||
        FAILED(hr = client_->GetService(
                   __uuidof(IAudioClock),
                   reinterpret_cast<void**>(clock_.GetAddressOf()))) ||
        FAILED(hr = clock_->GetFrequency(&clock_frequency_))) {
      *error = "IAudioClient setup";
      return hr;
    }
    REFERENCE_TIME latency = 0;
    if (SUCCEEDED(client_->GetStreamLatency(&latency))) {
      latency_ = latency / 10;
    }
    LOGI("opened: %d Hz, %d ch, buffer %u frames, latency %lld us",
         sample_rate_, kChannels, buffer_frames_,
         static_cast<long long>(latency_));
    return S_OK;
  }

  // When the first frame written now will be heard. The clock says which
  // frame was at the speaker at a QPC time; the frames written since are
  // still queued. Same derivation as Chromium's
  // media/audio/win/audio_low_latency_output_win.cc. The position already
  // includes the device latency, so the stream latency is not added again.
  int64_t PlayoutTime(UINT32 padding) const {
    UINT64 position = 0;
    UINT64 qpc_position = 0;
    if (clock_frequency_ > 0 &&
        SUCCEEDED(clock_->GetPosition(&position, &qpc_position))) {
      const uint64_t played =
          position * static_cast<uint64_t>(sample_rate_) / clock_frequency_;
      if (played <= frames_written_) {
        const int64_t queued = static_cast<int64_t>(frames_written_ - played);
        const int64_t at = static_cast<int64_t>(qpc_position / 10);
        return NowMicros() + (at - QpcNowMicros()) +
               queued * kMicrosPerSecond / sample_rate_;
      }
    }
    // No position yet: what is queued plays after the stream latency.
    return NowMicros() + latency_ +
           static_cast<int64_t>(padding) * kMicrosPerSecond / sample_rate_;
  }

  ComPtr<IAudioClient> client_;
  ComPtr<IAudioRenderClient> render_;
  ComPtr<IAudioClock> clock_;
  int sample_rate_ = 0;
  UINT32 buffer_frames_ = 0;
  UINT64 clock_frequency_ = 0;
  int64_t latency_ = 0;
  // Every frame handed to the device, silence included.
  uint64_t frames_written_ = 0;
};

// Flags default render device changes for the render thread. COM calls it
// on its own threads.
class WasapiSink::DeviceListener : public IMMNotificationClient {
 public:
  explicit DeviceListener(WasapiSink* sink) : sink_(sink) {}

  // The listener lives on the render thread's stack and is unregistered
  // before it goes away, so it is not reference counted.
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *object = static_cast<IMMNotificationClient*>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow,
                                                   ERole role,
                                                   LPCWSTR) override {
    if (flow == eRender && role == eConsole) {
      sink_->device_changed_ = true;
      SetEvent(sink_->wake_event_);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,
                                                   const PROPERTYKEY) override {
    return S_OK;
  }

 private:
  WasapiSink* const sink_;
};

WasapiSink::~WasapiSink() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
  }
  if (wake_event_) {
    SetEvent(wake_event_);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (wake_event_) {
    CloseHandle(wake_event_);
  }
}

bool WasapiSink::Open(RenderCallback callback, std::string* error) {
  callback_ = std::move(callback);
  wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!wake_event_) {
    *error = "CreateEvent failed";
    return false;
  }
  thread_ = std::thread(&WasapiSink::Run, this, error);
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return open_finished_; });
  if (quit_) {
    lock.unlock();
    thread_.join();
    return false;
  }
  return true;
}

void WasapiSink::Start() {
  SetStarted(true);
}

void WasapiSink::Stop() {
  SetStarted(false);
}

void WasapiSink::SetStarted(bool started) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!open_finished_ || quit_) {
    return;
  }
  want_started_ = started;
  const uint64_t command = ++commands_sent_;
  SetEvent(wake_event_);
  condition_.wait(lock,
                  [&] { return quit_ || commands_applied_ >= command; });
}

void WasapiSink::Run(std::string* open_error) {
  SetCurrentThreadName("lychee-wasapi");
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD task_index = 0;
  HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &task_index);
  HANDLE audio_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  ComPtr<IMMDeviceEnumerator> enumerator;
  DeviceListener listener(this);
  std::unique_ptr<Stream> stream;
  std::string error;
  if (!audio_event) {
    error = "CreateEvent failed";
  } else if (FAILED(CoCreateInstance(
                 __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                 __uuidof(IMMDeviceEnumerator),
                 reinterpret_cast<void**>(enumerator.GetAddressOf())))) {
    error = "cannot create IMMDeviceEnumerator";
  } else {
    stream = Stream::Create(enumerator.Get(), 0, audio_event, &error);
  }
  const bool opened = stream != nullptr;
  if (opened) {
    format_ = {stream->sample_rate(), kChannels};
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(&listener))) {
      LOGW("cannot follow default output device changes");
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    open_finished_ = true;
    if (!opened) {
      *open_error = error;
      quit_ = true;
    }
  }
  condition_.notify_all();

  bool started = false;
  int64_t reopen_time = 0;
  bool reopen_failed = false;
  while (opened) {
    HANDLE handles[] = {static_cast<HANDLE>(wake_event_), audio_event};
    WaitForMultipleObjects(2, handles, FALSE, kPollMillis);
    bool want_started;
    uint64_t command;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (quit_) {
        break;
      }
      want_started = want_started_;
      command = commands_sent_;
    }

    if (device_changed_.exchange(false) && stream) {
      LOGI("default output device changed");
      stream.reset();
      reopen_time = 0;
    }
    if (!stream && NowMicros() >= reopen_time) {
      std::string reopen_error;
      stream = Stream::Create(enumerator.Get(), format_.sample_rate,
                              audio_event, &reopen_error);
      started = false;
      if (stream) {
        reopen_failed = false;
      } else {
        if (!reopen_failed) {
          LOGW("reopen failed, retrying: %s", reopen_error.c_str());
        }
        reopen_failed = true;
        reopen_time = NowMicros() + kReopenInterval;
      }
    }
    if (stream && started != want_started) {
      const HRESULT hr =
          want_started ? stream->Start(callback_) : stream->Stop();
      if (FAILED(hr)) {
        LOGW("%s failed: 0x%08lx", want_started ? "Start" : "Stop",
             HResultCode(hr));
      }
      started = want_started;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      commands_applied_ = command;
    }
    condition_.notify_all();

    if (stream && started) {
      const HRESULT hr = stream->Fill(callback_);
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        LOGI("output device went away");
        stream.reset();
        reopen_time = 0;
        SetEvent(wake_event_);
      } else if (FAILED(hr)) {
        LOGW("render failed: 0x%08lx", HResultCode(hr));
      }
    }
  }

  if (opened) {
    enumerator->UnregisterEndpointNotificationCallback(&listener);
  }
  stream.reset();
  enumerator.Reset();
  if (audio_event) {
    CloseHandle(audio_event);
  }
  if (task) {
    AvRevertMmThreadCharacteristics(task);
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
}

}  // namespace lychee
