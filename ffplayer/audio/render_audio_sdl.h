//
// Created by boyan on 21-2-17.
//

#ifndef ANDROID_RENDER_AUDIO_SDL_H
#define ANDROID_RENDER_AUDIO_SDL_H

#include "render_audio_base.h"
extern "C" {
#include "SDL.h"
};

class SdlAudioRender : public AudioRenderBase {

 private:
  bool mute_ = false;
  int audio_volume_ = 100;
  SDL_AudioDeviceID audio_device_id_ = -1;

  /* current context */
  int64_t audio_callback_time = 0;

 private:
  void AudioCallback(Uint8 *stream, int len);

 protected:
  int OnBeforeDecodeFrame() override;
 public:

  SdlAudioRender();

  void Init(const std::shared_ptr<PacketQueue> &audio_queue, std::shared_ptr<MediaClock> clock_ctx) override;

  int Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) override;
  void Start() const override;
  void Pause() const override;
  bool IsMute() const override;
  void SetMute(bool mute) override;
  void SetVolume(int _volume) override;
  int GetVolume() const override;
};

#endif //ANDROID_RENDER_AUDIO_SDL_H
