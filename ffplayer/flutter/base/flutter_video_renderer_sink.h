//
// Created by yangbin on 2021/5/6.
//

#ifndef MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_

#include "base/task_runner.h"

#include "video_renderer_sink.h"

namespace media {

class FlutterVideoRendererSink : public VideoRendererSink {

 public:

  FlutterVideoRendererSink() = default;

  ~FlutterVideoRendererSink() override = default;

  virtual int64_t Attach() = 0;

  virtual void Detach() = 0;

 private:

  DELETE_COPY_AND_ASSIGN(FlutterVideoRendererSink);

};

} // namespace media

#endif //MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_
