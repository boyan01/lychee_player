//
// Created by boyan on 2021/5/13.
//

#ifndef MEDIA_FLUTTER_BASE_NULL_VIDEO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_BASE_NULL_VIDEO_RENDERER_SINK_H_

#include "video_renderer_sink.h"

namespace media {

class NullVideoRendererSink : public VideoRendererSink {

 public:

  void Start(RenderCallback *callback) override;

  void Stop() override;

};

}

#endif //MEDIA_FLUTTER_BASE_NULL_VIDEO_RENDERER_SINK_H_
