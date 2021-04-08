//
// Created by yangbin on 2021/4/7.
//

#ifndef MEDIA_SDL_SDL_AUDIO_RENDER_H_
#define MEDIA_SDL_SDL_AUDIO_RENDER_H_

#include "SDL2/SDL.h"

#include "renderer/audio_renderer.h"

namespace media {

class SdlAudioRender : public AudioRenderer {

 public:

  void StartPlaying() override;

  void Flush() override;

 protected:

  void DoInitialize(PipelineStatusCallback init_callback) override;

 private:

  // the audio_device_id_ which we opened.
  SDL_AudioDeviceID audio_device_id_ = -1;

  /**
   * Open SDL audio device.
   *
   * @return the size of audio buffer. which <= 0 meaning failed.
   */
  int OpenAudioDevice();

};

}

#endif //MEDIA_SDL_SDL_AUDIO_RENDER_H_
