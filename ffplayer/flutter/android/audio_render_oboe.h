//
// Created by yangbin on 2021/3/6.
//

#ifndef ANDROID_FFPLAYER_FLUTTER_ANDROID_AUDIO_RENDER_OBOE_H_
#define ANDROID_FFPLAYER_FLUTTER_ANDROID_AUDIO_RENDER_OBOE_H_

#include <functional>

#include "oboe/Oboe.h"

#include "audio_render_basic.h"

namespace media {

class AudioRenderOboe : public BasicAudioRender, public oboe::AudioStreamDataCallback {

 private:
  std::shared_ptr<oboe::AudioStream> audio_stream_;

 public:

  AudioRenderOboe();

  virtual ~AudioRenderOboe();

  void Start() const override;

  void Pause() const override;

  oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override;

 protected:

  int OpenAudioDevice(int64_t wanted_channel_layout,
                      int wanted_nb_channels,
                      int wanted_sample_rate,
                      AudioParams &device_output) override;

};

}  // namespace media

#endif //ANDROID_FFPLAYER_FLUTTER_ANDROID_AUDIO_RENDER_OBOE_H_
