//
// Created by yangbin on 2021/5/1.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_RENDERER_H_
#define MEDIA_PLAYER_SRC_AUDIO_RENDERER_H_

#include "memory"

#include "base/basictypes.h"
#include "demuxer_stream.h"
#include "media_clock.h"
#include "audio_decoder.h"
#include "decoder_stream.h"
#include "audio_renderer_sink.h"

namespace media {

class AudioRenderer : public std::enable_shared_from_this<AudioRenderer>, public AudioRendererSink::RenderCallback {

 public:

  explicit AudioRenderer(TaskRunner *task_runner, std::shared_ptr<AudioRendererSink> sink);

  virtual ~AudioRenderer();

  using InitCallback = std::function<void(bool success)>;
  void Initialize(DemuxerStream *decoder_stream, std::shared_ptr<MediaClock> media_clock, InitCallback init_callback);

  void Start();

  void Stop();

  int Render(double delay, uint8 *stream, int len) override;

  void OnRenderError() override;

 private:

  TaskRunner *task_runner_;
  DemuxerStream *demuxer_stream_ = nullptr;

  std::shared_ptr<AudioDecoderStream> decoder_stream_;

  std::shared_ptr<AudioRendererSink> sink_;

  std::shared_ptr<MediaClock> media_clock_;

  CircularDeque<std::shared_ptr<AudioBuffer>> audio_buffer_;

  InitCallback init_callback_;

  void OnDecoderStreamInitialized(bool success);

  void AttemptReadFrame();

  void OnNewFrameAvailable(AudioDecoderStream::ReadResult result);

  bool NeedReadStream();

  DISALLOW_COPY_AND_ASSIGN(AudioRenderer);

};

}
#endif //MEDIA_PLAYER_SRC_AUDIO_RENDERER_H_
