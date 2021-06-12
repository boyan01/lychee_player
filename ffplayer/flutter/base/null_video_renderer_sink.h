//
// Created by boyan on 2021/5/13.
//

#ifndef MEDIA_FLUTTER_BASE_NULL_VIDEO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_BASE_NULL_VIDEO_RENDERER_SINK_H_

#include "flutter_video_renderer_sink.h"

namespace media {

class NullVideoRendererSink : public FlutterVideoRendererSink {

 public:

  void Start(RenderCallback *callback) override;

  void Stop() override;

  int64_t Attach() override;
  void Detach() override;

};

}

#endif //MEDIA_FLUTTER_BASE_NULL_VIDEO_RENDERER_SINK_H_
