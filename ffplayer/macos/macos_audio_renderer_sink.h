//
// Created by Bin Yang on 2021/5/23.
//

#ifndef MEDIA_FLUTTER_MACOS_MACOS_AUDIO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_MACOS_MACOS_AUDIO_RENDERER_SINK_H_

#include "AudioToolbox/AudioQueue.h"

#include "mutex"

#include "audio_renderer_sink.h"

#include "base/task_runner.h"

namespace media {

class MacosAudioRendererSink : public AudioRendererSink {

 public:

  MacosAudioRendererSink();

  ~MacosAudioRendererSink() override;

  void Initialize(int in_user_data, int audio_queue, RenderCallback *callback) override;

  bool SetVolume(double volume) override;

  void Play() override;

  void Pause() override;

  uint32 ReadAudioData(uint8 *stream, uint32 len);

 private:

  enum State {
    kIdle,
    kPlaying,
    kPaused,
  };

  AudioQueueRef audio_queue_ = nullptr;

  RenderCallback *render_callback_ = nullptr;

  // size of buffer_;
  int buffer_size_;
  uint32 buffer_offset_;
  uint8 *buffer_;

  std::vector<AudioQueueBufferRef> audio_buffer_;

  std::mutex mutex_;

  State state_;

  void InitializeBuffer(int audio_buffer_nb);

  DELETE_COPY_AND_ASSIGN(MacosAudioRendererSink);

};

}

#endif //MEDIA_FLUTTER_MACOS_MACOS_AUDIO_RENDERER_SINK_H_
