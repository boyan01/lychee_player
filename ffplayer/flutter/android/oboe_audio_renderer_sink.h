//
// Created by yangbin on 2021/5/6.
//

#ifndef MEDIA_FLUTTER_ANDROID_OBOE_AUDIO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_ANDROID_OBOE_AUDIO_RENDERER_SINK_H_

#include "audio_renderer_sink.h"
#include "oboe/Oboe.h"

namespace media {

class OboeAudioRendererSink : public AudioRendererSink,
                              public oboe::AudioStreamDataCallback {
 public:
  virtual ~OboeAudioRendererSink();

  void Initialize(int wanted_nb_channels, int wanted_sample_rate,
                  RenderCallback *render_callback) override;

  bool SetVolume(double volume) override;

  void Play() override;

  void Pause() override;

  oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream,
                                        void *audioData,
                                        int32_t numFrames) override;

 private:
  RenderCallback *render_callback_;

  std::shared_ptr<oboe::AudioStream> audio_stream_;
};

}  // namespace media

#endif  // MEDIA_FLUTTER_ANDROID_OBOE_AUDIO_RENDERER_SINK_H_
