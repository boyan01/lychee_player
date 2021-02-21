//
// Created by boyan on 2021/2/21.
//


#ifndef ANDROID_ANDROID_SRC_MAIN_CPP_FFPLAYER_SRC_RENDER_AUDIO_ANDROID_H_
#define ANDROID_ANDROID_SRC_MAIN_CPP_FFPLAYER_SRC_RENDER_AUDIO_ANDROID_H_

#if defined(_FLUTTER_ANDROID)

#include <memory>

#include "render_audio_base.h"

namespace media {

class AndroidAudioRender : public AudioRenderBase {

 private:

 public:

  AndroidAudioRender();

  virtual ~AndroidAudioRender();

  int Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) override;

  void Start() const override;

  void Pause() const override;

  bool IsMute() const override;

  void SetMute(bool _mute) override;

  void SetVolume(int _volume) override;

  int GetVolume() const override;

  void Init(const std::shared_ptr<PacketQueue> &audio_queue, std::shared_ptr<ClockContext> clock_ctx) override;

  void Abort() override;
 protected:
  int OnBeforeDecodeFrame() override;

};

}

#endif //_FLUTTER_ANDROID

#endif //ANDROID_ANDROID_SRC_MAIN_CPP_FFPLAYER_SRC_RENDER_AUDIO_ANDROID_H_