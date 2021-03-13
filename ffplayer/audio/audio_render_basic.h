//
// Created by yangbin on 2021/3/6.
//

#ifndef MEDIA_AUDIO_AUDIO_RENDER_BASIC_H_
#define MEDIA_AUDIO_AUDIO_RENDER_BASIC_H_

#include <thread>
#include <mutex>

#include "render_audio_base.h"

class BasicAudioRender : public AudioRenderBase {

 private:
  static const int MAX_AUDIO_VOLUME = 100;

  static void MixAudioVolume(uint8_t *dst,
                             const uint8_t *src,
                             uint32_t len, int volume);

 private:
  bool mute_ = false;
  int audio_volume_ = MAX_AUDIO_VOLUME;
  /* current context */
  int64_t audio_callback_time_ = 0;

 protected:

  virtual int OpenAudioDevice(int64_t wanted_channel_layout,
                              int wanted_nb_channels,
                              int wanted_sample_rate,
                              AudioParams &device_output) = 0;

  /**
   *  callback to read more audio frame data.
   * @param stream output source.
   * @param len length of bytes to be read.
   */
  void ReadAudioData(uint8_t *stream, int len);

  int OnBeforeDecodeFrame() override;

 public:
  int Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) override;

  bool IsMute() const override;

  void SetMute(bool _mute) override;

  void SetVolume(int _volume) override;

  int GetVolume() const override;

  bool IsReady() override;

};

#endif //MEDIA_AUDIO_AUDIO_RENDER_BASIC_H_
