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

  FlutterVideoRendererSink();

  virtual ~FlutterVideoRendererSink();

  void Start(RenderCallback *callback) override;

  void Stop() override;

  virtual int64_t Attach() = 0;

  virtual void Detach() = 0;

 protected:

  virtual void DoRender(std::shared_ptr<VideoFrame> frame) = 0;

 private:

  enum State { kIdle, kRunning };
  State state_ = kIdle;

  // FIXME: maybe we can share with a global looper?
  base::MessageLooper *looper_;

  TaskRunner task_runner_;
  RenderCallback *render_callback_;

  void RenderTask();

  DELETE_COPY_AND_ASSIGN(FlutterVideoRendererSink);

};

} // namespace media

#endif //MEDIA_FLUTTER_BASE_FLUTTER_VIDEO_RENDERER_SINK_H_
