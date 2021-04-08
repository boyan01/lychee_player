//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_RENDERER_VIDEO_RENDERER_H_
#define MEDIA_PLAYER_RENDERER_VIDEO_RENDERER_H_

#include "pipeline/pipeline_status.h"
#include "demuxer/demuxer_stream.h"

namespace media {

class VideoRenderer {

 public:

  void Initialize(DemuxerStream *audio_stream, PipelineStatusCallback init_callback);

};

} // namespace media

#endif //MEDIA_PLAYER_RENDERER_VIDEO_RENDERER_H_
