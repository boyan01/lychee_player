//
// Created by yangbin on 2021/10/6.
//

#ifndef MEDIA_AUDIO_WASAPI_AUDIO_RENDER_SINK_H_
#define MEDIA_AUDIO_WASAPI_AUDIO_RENDER_SINK_H_

#include "base/task_runner.h"

#include "audio_renderer_sink.h"

#include <MMDeviceAPI.h>
#include <AudioClient.h>
#include <AudioPolicy.h>

namespace media {

// TODO IMPL
class WasapiAudioRenderSink : public AudioRendererSink {

 public:

  WasapiAudioRenderSink();

  ~WasapiAudioRenderSink() override;

  void Initialize(int wanted_nb_channels, int wanted_sample_rate, RenderCallback *render_callback) override;

  bool SetVolume(double volume) override;

  void Play() override;

  void Pause() override;

 private:

  RenderCallback *render_callback_;

  IMMDeviceEnumerator *device_enumerator_ = nullptr;
  //
  //  Core Audio Rendering member variables.
  //
  IMMDevice *end_point_;
  IAudioClient *audio_client_;
  IAudioRenderClient *render_client_;

  WAVEFORMATEX *wave_format_;

  /* this is in sample frames, not samples, not bytes. */
  UINT32 buffer_size_;

  HANDLE event_ = nullptr;

  bool playing_ = false;

  uint16 samples_ = 0;

  HANDLE render_thread_ = nullptr;

  bool InitializeAudioEngine();

  void StartRender();

  static DWORD __stdcall RenderThread(LPVOID context);

  void WaitDevice();

  void DoRenderThread();

};

}

#endif //MEDIA_AUDIO_WASAPI_AUDIO_RENDER_SINK_H_
