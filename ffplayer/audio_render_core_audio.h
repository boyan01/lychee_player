//
// Created by Bin Yang on 2022/8/7.
//

#ifndef MEDIA__AUDIO_RENDER_CORE_AUDIO_H_
#define MEDIA__AUDIO_RENDER_CORE_AUDIO_H_

#ifdef LYCHEE_OSX

#include <vector>

#include "audio_render_basic.h"

#include "AudioToolbox/AudioQueue.h"

class AudioRenderCoreAudio : public BasicAudioRender {
 private:
  AudioQueueRef audio_queue_ = nullptr;

  std::vector<AudioQueueBufferRef> audio_buffer_;

  int buffer_size_;

  void InitializeBuffer(int audio_buffer_nb);

 protected:
  int OpenAudioDevice(int64_t wanted_channel_layout,
                      int wanted_nb_channels,
                      int wanted_sample_rate,
                      AudioParams& device_output) override;

  void OnStart() const override;
  void onStop() const override;


};

#endif

#endif  // MEDIA__AUDIO_RENDER_CORE_AUDIO_H_
