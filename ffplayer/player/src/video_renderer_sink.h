//
// Created by yangbin on 2021/5/5.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_RENDERER_SINK_H_
#define MEDIA_PLAYER_SRC_VIDEO_RENDERER_SINK_H_

#include "base/time_delta.h"
#include "video_frame.h"

namespace media {

class VideoRendererSink {
 public:
  class RenderCallback {
   public:
    virtual std::shared_ptr<VideoFrame> Render(TimeDelta &next_frame_delay) = 0;

    virtual void OnFrameDrop() = 0;

    virtual ~RenderCallback() = default;
  };

  virtual void Start(RenderCallback *callback) = 0;

  virtual void Stop() = 0;

  virtual ~VideoRendererSink() = default;
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_VIDEO_RENDERER_SINK_H_
