//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_RENDERER_AUDIO_RENDERER_H_
#define MEDIA_PLAYER_RENDERER_AUDIO_RENDERER_H_

#include "pipeline/pipeline_status.h"
#include "demuxer/demuxer_stream.h"

namespace media {

class AudioRenderer {

 public:

  void Initialize(DemuxerStream *audio_stream, PipelineStatusCallback init_callback);

  virtual void StartPlaying() = 0;

  virtual void Flush() = 0;

 protected:

  DemuxerStream *stream_ = nullptr;

  virtual void DoInitialize(PipelineStatusCallback init_callback) = 0;

 private:

  DISALLOW_COPY_AND_ASSIGN(AudioRenderer);

};

} // namespace media

#endif //MEDIA_PLAYER_RENDERER_AUDIO_RENDERER_H_
