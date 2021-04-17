//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_RENDERER_RENDERER_H_
#define MEDIA_PLAYER_RENDERER_RENDERER_H_

#include "memory"

#include "base/basictypes.h"
#include "base/message_loop.h"

#include "demuxer/demuxer.h"
#include "renderer/audio_renderer.h"
#include "renderer/video_renderer.h"
#include "pipeline/pipeline_status.h"

namespace media {

class Renderer : std::enable_shared_from_this<Renderer> {

 public:

  Renderer(std::shared_ptr<base::MessageLoop> task_runner,
           std::unique_ptr<AudioRenderer> audio_renderer,
           std::unique_ptr<VideoRenderer> video_renderer);

  void Initialize(Demuxer *media_source, PipelineStatusCallback init_callback);

 private:

  enum State {
    STATE_UNINITIALIZED,
    STATE_INIT_PENDING_CDM,  // Initialization is waiting for the CDM to be set.
    STATE_INITIALIZING,      // Initializing audio/video renderers.
    STATE_FLUSHING,          // Flushing is in progress.
    STATE_FLUSHED,           // After initialization or after flush completed.
    STATE_PLAYING,           // After StartPlayingFrom has been called.
    STATE_ERROR
  };

  std::unique_ptr<AudioRenderer> audio_renderer_;
  std::unique_ptr<VideoRenderer> video_renderer_;

  std::shared_ptr<base::MessageLoop> task_runner_;

  PipelineStatusCallback init_callback_;

  Demuxer *media_source_ = nullptr;

  State state_ = STATE_UNINITIALIZED;

  DemuxerStream *current_audio_stream_ = nullptr;
  DemuxerStream *current_video_stream_ = nullptr;

  void InitializeAudioRender();

  void InitializeVideoRender();

  void OnAudioRendererInitializeDone(PipelineStatus status);

  void OnVideoRendererInitializeDone(PipelineStatus status);

  void FinishInitialization(PipelineStatus status);

  DISALLOW_COPY_AND_ASSIGN(Renderer);
};

}

#endif //MEDIA_PLAYER_RENDERER_RENDERER_H_
