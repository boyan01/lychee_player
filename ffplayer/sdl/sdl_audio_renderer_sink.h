//
// Created by yangbin on 2021/5/3.
//

#ifndef MEDIA_SDL_SDL_AUDIO_RENDERER_SINK_H_
#define MEDIA_SDL_SDL_AUDIO_RENDERER_SINK_H_

#include "audio_renderer_sink.h"

#include "SDL2/SDL.h"

namespace media {

class SdlAudioRendererSink : public AudioRendererSink {

 public:

  void Initialize(int wanted_nb_channels, int wanted_sample_rate, RenderCallback *render_callback) override;

  bool SetVolume(double volume) override;

  void Play() override;
  void Pause() override;

  ~SdlAudioRendererSink() override;

 private:

  RenderCallback *render_callback_;

  SDL_AudioDeviceID audio_device_id_;

  int hw_audio_buffer_size_;
  int sample_rate_;

  int OpenAudioDevices(int wanted_nb_channels, int wanted_sample_rate);

  void ReadAudioData(Uint8 *stream, int len);

};

}

#endif //MEDIA_SDL_SDL_AUDIO_RENDERER_SINK_H_
