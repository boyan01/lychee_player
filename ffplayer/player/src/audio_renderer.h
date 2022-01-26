//
// Created by yangbin on 2021/5/1.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_RENDERER_H_
#define MEDIA_PLAYER_SRC_AUDIO_RENDERER_H_

#include <ostream>

#include "audio_decoder.h"
#include "audio_renderer_sink.h"
#include "base/basictypes.h"
#include "decoder_stream.h"
#include "demuxer_stream.h"
#include "media_clock.h"
#include "memory"

namespace media {

class AudioRenderer : public std::enable_shared_from_this<AudioRenderer>,
                      public AudioRendererSink::RenderCallback {
 public:
  AudioRenderer(std::shared_ptr<TaskRunner> task_runner,
                std::shared_ptr<AudioRendererSink> sink);

  ~AudioRenderer() override;

  using InitCallback = std::function<void(bool success)>;
  void Initialize(DemuxerStream *decoder_stream,
                  std::shared_ptr<MediaClock> media_clock,
                  InitCallback init_callback);

  void Start();

  void Stop();

  int Render(double delay, uint8 *stream, int len) override;

  void OnRenderError() override;

  void SetVolume(double volume);

  double GetVolume() const { return volume_; };

  void Flush();

  void MarkStatePlaying();

  friend std::ostream &operator<<(std::ostream &os,
                                  const AudioRenderer &renderer);

 private:
  enum State {
    kUnInitialized,
    kInitializing,
    kFlushed,
    kPlaying,
    kStopped
  };

  State state_ = kUnInitialized;

  std::shared_ptr<TaskRunner> task_runner_;
  DemuxerStream *demuxer_stream_ = nullptr;

  std::shared_ptr<AudioDecoderStream> decoder_stream_;

  std::shared_ptr<AudioRendererSink> sink_;

  std::shared_ptr<MediaClock> media_clock_;

  std::deque<std::shared_ptr<AudioBuffer>> audio_buffer_;

  InitCallback init_callback_;

  bool reading_ = false;

  std::mutex mutex_;

  double volume_;

  void OnDecoderStreamInitialized(bool success);

  void AttemptReadFrame();

  void OnNewFrameAvailable(AudioDecoderStream::ReadResult result);

  bool NeedReadStream();

  DELETE_COPY_AND_ASSIGN(AudioRenderer);
};

}  // namespace media
#endif  // MEDIA_PLAYER_SRC_AUDIO_RENDERER_H_
