//
// Created by Bin Yang on 2022/5/4.
//

#ifndef LYCHEE_PLAYER__SDL_AUDIO_RENDERER_H_
#define LYCHEE_PLAYER__SDL_AUDIO_RENDERER_H_

#include "audio_renderer.h"

extern "C" {
#include "SDL2/SDL.h"
}

namespace lychee {

class SdlAudioRenderer : public AudioRenderer {

 public:

  SdlAudioRenderer(const media::TaskRunner &task_runner, AudioRendererHost *host);

  void Play() override;

  void Pause() override;

  ~SdlAudioRenderer() override;

 protected:

  bool OpenDevice(const AudioDecodeConfig &decode_config) override;

 private:
  SDL_AudioDeviceID audio_device_id_;

  bool is_playing_;

  void ReadAudioData(Uint8 *stream, int length);
};

}

#endif //LYCHEE_PLAYER__SDL_AUDIO_RENDERER_H_
