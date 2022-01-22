//
// Created by yangbin on 2021/5/3.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_RENDERER_SINK_H_
#define MEDIA_PLAYER_SRC_AUDIO_RENDERER_SINK_H_

#include "base/basictypes.h"

namespace media {

class AudioRendererSink {
 public:
  class RenderCallback {
   public:
    virtual int Render(double delay, uint8 *stream, int len) = 0;

    virtual void OnRenderError() = 0;

   protected:
    virtual ~RenderCallback() = default;
  };

  virtual void Initialize(int wanted_nb_channels, int wanted_sample_rate,
                          RenderCallback *render_callback) = 0;

  /**
   * @param volume range in [0, 1].
   * @return true on success.
   */
  virtual bool SetVolume(double volume) = 0;

  /**
   * 开始播放音乐。如果当前处于播放状态，则无反应。
   *
   * 必须 [Initialize] 之后才能调用。
   */
  virtual void Play() = 0;

  /**
   * 当处于播放时，暂停播放音乐
   *
   * 必须 [Initialize] 之后才能调用。
   */
  virtual void Pause() = 0;

  virtual ~AudioRendererSink() = default;
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_AUDIO_RENDERER_SINK_H_
