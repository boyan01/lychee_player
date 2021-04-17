//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_RENDERER_VIDEO_RENDERER_H_
#define MEDIA_PLAYER_RENDERER_VIDEO_RENDERER_H_

#include "mutex"

#include "pipeline/pipeline_status.h"
#include "demuxer/demuxer_stream.h"
#include "decoder/video_decoder.h"
#include "decoder/decoder_stream.h"

namespace media {

class VideoRenderer {

 public:

  void Initialize(DemuxerStream *audio_stream, PipelineStatusCallback init_callback);

 private:

  // Provides video frames to VideoRenderer.
  std::unique_ptr<VideoDecoderStream> video_decoder_stream_;

  // Passed in during Initialize().
  DemuxerStream* demuxer_stream_;

  std::shared_ptr<base::MessageLoop> task_runner_;

  // Used for accessing data members.
  std::mutex mutex_;

  // Important detail: being in kPlaying doesn't imply that video is being
  // rendered. Rather, it means that the renderer is ready to go. The actual
  // rendering of video is controlled by time advancing via |get_time_cb_|.
  // Video renderer can be reinitialized completely by calling Initialize again
  // when it is in a kFlushed state with video sink stopped.
  //
  //    kUninitialized
  //  +------> | Initialize()
  //  |        |
  //  |        V
  //  |   kInitializing
  //  |        | Decoders initialized
  //  |        |
  //  |        V            Decoders reset
  //  ---- kFlushed <------------------ kFlushing
  //           | StartPlayingFrom()         ^
  //           |                            |
  //           |                            | Flush()
  //           `---------> kPlaying --------'
  enum State { kUninitialized, kInitializing, kFlushing, kFlushed, kPlaying };
  State state_;

  PipelineStatusCallback init_cb_;

};

} // namespace media

#endif //MEDIA_PLAYER_RENDERER_VIDEO_RENDERER_H_
