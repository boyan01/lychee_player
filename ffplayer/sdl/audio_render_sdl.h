//
// Created by yangbin on 2021/3/6.
//

#ifndef MEDIA_AUDIO_AUDIO_RENDER_SDL_2_H_
#define MEDIA_AUDIO_AUDIO_RENDER_SDL_2_H_

#include "audio_render_basic.h"

extern "C" {
#include "SDL2/SDL.h"
};

namespace media {

class AudioRenderSdl : public BasicAudioRender {

 private:
  SDL_AudioDeviceID audio_device_id_ = -1;
 public:

  ~AudioRenderSdl() override;

 protected:

  void OnStart() const override;

  void onStop() const override;

  int OpenAudioDevice(int64_t wanted_channel_layout,
                      int wanted_nb_channels,
                      int wanted_sample_rate,
                      AudioParams &device_output) override;

};

}

#endif //MEDIA_AUDIO_AUDIO_RENDER_SDL_2_H_
