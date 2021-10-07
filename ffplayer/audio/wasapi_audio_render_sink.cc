//
// Created by yangbin on 2021/10/6.
//


#include "base/logging.h"
#include <base/utility.h>

#include "wasapi_audio_render_sink.h"

#undef ERROR

namespace {

}

namespace media {

WasapiAudioRenderSink::WasapiAudioRenderSink() :
    audio_client_(nullptr),
    render_client_(nullptr),
    buffer_size_(0),
    render_callback_(nullptr),
    end_point_(nullptr),
    wave_format_(nullptr) {

  auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&device_enumerator_));
  DCHECK(device_enumerator_) << "Unable to instantiate device enumerator:" << hr;
  hr = device_enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &end_point_);
  DCHECK(end_point_) << "unable to get default audio endpoint: " << hr;
}

WasapiAudioRenderSink::~WasapiAudioRenderSink() {
  if (render_thread_) {
    WaitForSingleObject(render_thread_, INFINITE);

    CloseHandle(render_thread_);
    render_thread_ = nullptr;
  }

  if (event_) {
    CloseHandle(event_);
  }

  if (device_enumerator_) {
    device_enumerator_->Release();
  }

  if (end_point_) {
    end_point_->Release();
  }

}

void WasapiAudioRenderSink::Initialize(int wanted_nb_channels,
                                       int wanted_sample_rate,
                                       AudioRendererSink::RenderCallback *render_callback) {
  render_callback_ = render_callback;

  auto hr = end_point_->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER,
                                 nullptr, reinterpret_cast<void **>(&audio_client_));
  if (FAILED(hr)) {
    DLOG(INFO) << "unable to activate audio client: " << hr;
    return;
  }
  DCHECK(audio_client_);

  hr = audio_client_->GetMixFormat(&wave_format_);
  if (FAILED(hr)) {
    DLOG(WARNING) << "can't determine mix format";
    return;
  }

  DLOG(INFO) << "audio format:" << (wave_format_->wFormatTag) << ", bites " << wave_format_->wBitsPerSample;

  event_ = CreateEventEx(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
  if (event_ == nullptr) {
    DLOG(WARNING) << "can not create event handle.";
    return;
  }

  REFERENCE_TIME default_period = 0;
  hr = audio_client_->GetDevicePeriod(&default_period, nullptr);
  if (FAILED(hr)) {
    LOG(WARNING) << "can't determine minimum device period";
    return;
  }

  if (InitializeAudioEngine()) {
    StartRender();
  }

  /* Match the callback size to the period size to cut down on the number of
   interrupts waited for in each call to WaitDevice */
  {
    const float period_millis = (float) default_period / 10000.0f;
    const float period_frames = period_millis * float(wave_format_->nSamplesPerSec) / 1000.0f;
    samples_ = (uint16) period_frames;
  }

}

bool WasapiAudioRenderSink::InitializeAudioEngine() {
  DCHECK(audio_client_);
  auto hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                                      10,
                                      0,
                                      wave_format_,
                                      nullptr);
  if (FAILED(hr)) {
    DLOG(WARNING) << "Unable to initialize audio client:" << hr;
    return false;
  }

  hr = audio_client_->GetBufferSize(&buffer_size_);
  if (FAILED(hr)) {
    DLOG(WARNING) << "Unable to get audio client buffer: " << hr;
    return false;
  }

  hr = audio_client_->SetEventHandle(event_);
  if (FAILED(hr)) {
    DLOG(ERROR) << "Unable to set ready event: " << hr;
    return false;
  }

  hr = audio_client_->GetService(IID_PPV_ARGS(&render_client_));
  if (FAILED(hr)) {
    DLOG(WARNING) << "Unable to get new render client: " << hr;
    return false;
  }
  return true;
}

void WasapiAudioRenderSink::StartRender() {
  //
  //  We want to pre-roll one buffer's worth of silence into the pipeline.  That way the audio engine won't glitch on startup.
  //  We pre-roll silence instead of audio buffers because our buffer size is significantly smaller than the engine latency
  //  and we can only pre-roll one buffer's worth of audio samples.
  //
  //
  {
    BYTE *data;
    auto hr = render_client_->GetBuffer(buffer_size_, &data);
    if (FAILED(hr)) {
      DLOG(WARNING) << "failed to get buffer: " << hr;
      return;
    }
    hr = render_client_->ReleaseBuffer(buffer_size_, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
      DLOG(WARNING) << "failed to release buffer: " << hr;
      return;
    }
  }

  //
  //  Now create the thread which is going to drive the renderer.
  //
  render_thread_ = CreateThread(nullptr, 0, RenderThread, this, 0, nullptr);
  if (!render_thread_) {
    DLOG(WARNING) << "unable to create transport thread: " << GetLastError();
    return;
  }

  auto hr = audio_client_->Start();
  if (FAILED(hr)) {
    DLOG(WARNING) << "unable to start render client: " << hr;
  }

}

bool WasapiAudioRenderSink::SetVolume(double volume) {
  return false;
}

void WasapiAudioRenderSink::Play() {
  playing_ = true;
  audio_client_->Start();
}

void WasapiAudioRenderSink::Pause() {
  playing_ = false;
}

DWORD WasapiAudioRenderSink::RenderThread(LPVOID context) {
  auto *sink = static_cast<WasapiAudioRenderSink *>(context);
  utility::update_thread_name("audio_callback");
  sink->DoRenderThread();
  return 0;
}

void WasapiAudioRenderSink::WaitDevice() {
  while (audio_client_ && event_) {
    DWORD wait_result = WaitForSingleObjectEx(event_, 200, FALSE);
    if (wait_result == WAIT_OBJECT_0) {
      const UINT32 maxpadding = samples_;
      UINT32 padding = 0;
      if (SUCCEEDED(audio_client_->GetCurrentPadding(&padding))) {
        /*SDL_Log("WASAPI EVENT! padding=%u maxpadding=%u", (unsigned int)padding, (unsigned int)maxpadding);*/
//        DLOG(INFO) << "padding= " << padding << " maxpadding=" << maxpadding;
        if (padding <= maxpadding) {
          break;
        }
      }
    } else if (wait_result != WAIT_TIMEOUT) {
      audio_client_->Stop();
      playing_ = false;
      break;
    }
  }
}

void WasapiAudioRenderSink::DoRenderThread() {
  DLOG(INFO) << "DoRenderThread";
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  if (render_callback_ == nullptr) {
    return;
  }

  while (playing_) {
    WaitDevice();
    /* get an endpoint buffer from WASAPI. */
    BYTE *buffer = nullptr;

    auto hr = render_client_->GetBuffer(samples_, &buffer);
    if (FAILED(hr)) {
      DLOG(ERROR) << "failed to get render buffer: " << hr;
      continue;
    }
    DCHECK(buffer);

    // FIXME buffer size need more calculate.
    // len = samples * size_of_audio_bits * channel
    render_callback_->Render(0, buffer, samples_ * 4);
    render_client_->ReleaseBuffer(samples_, 0);
  }

}

}
