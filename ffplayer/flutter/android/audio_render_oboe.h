//
// Created by yangbin on 2021/3/6.
//

#ifndef ANDROID_FFPLAYER_FLUTTER_ANDROID_AUDIO_RENDER_OBOE_H_
#define ANDROID_FFPLAYER_FLUTTER_ANDROID_AUDIO_RENDER_OBOE_H_

#include <functional>

#include "oboe/Oboe.h"

#include "audio_render_basic.h"

namespace media {

class AudioCallback : public oboe::AudioStreamDataCallback {
 private:
  std::function<oboe::DataCallbackResult(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames)> callback_;

 public:

  AudioCallback(const std::function<oboe::DataCallbackResult(oboe::AudioStream *, void *, int32_t)> &callback)
      : callback_(callback) {}

  oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) override {
    return callback_(audioStream, audioData, numFrames);
  }
};

class AudioRenderOboe : public BasicAudioRender {

 private:
  std::shared_ptr<oboe::AudioStream> audio_stream_;
  AudioCallback *audio_callback_;

  oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream, void *audioData, int32_t numFrames);

 public:

  AudioRenderOboe();

  virtual ~AudioRenderOboe();

  void Start() const override;

  void Pause() const override;

 protected:

  int OpenAudioDevice(int64_t wanted_channel_layout,
                      int wanted_nb_channels,
                      int wanted_sample_rate,
                      AudioParams &device_output) override;

};

}  // namespace media

#endif //ANDROID_FFPLAYER_FLUTTER_ANDROID_AUDIO_RENDER_OBOE_H_
