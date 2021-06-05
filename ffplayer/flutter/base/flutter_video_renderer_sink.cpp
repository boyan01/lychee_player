//
// Created by yangbin on 2021/5/6.
//

#include "flutter_video_renderer_sink.h"

#include "base/logging.h"

namespace media {

FlutterVideoRendererSink::FlutterVideoRendererSink()
    : task_runner_(TaskRunner::prepare_looper("video_render")),
      render_callback_(nullptr) {

}

void FlutterVideoRendererSink::Start(VideoRendererSink::RenderCallback *callback) {
  DCHECK_EQ(state_, kIdle);
  render_callback_ = callback;

  task_runner_->PostTask(FROM_HERE, std::bind(&FlutterVideoRendererSink::RenderTask, this));
}

void FlutterVideoRendererSink::Stop() {
  DCHECK_EQ(state_, kRunning);
  task_runner_->PostTask(FROM_HERE, [&]() {
    render_callback_ = nullptr;
    state_ = kIdle;
  });
}

void FlutterVideoRendererSink::RenderTask() {
  if (render_callback_ == nullptr) {
    return;
  }
  auto frame = render_callback_->Render();
  if (!frame->IsEmpty()) {
    DoRender(std::move(frame));
  }
  // schedule next frame after 10 ms.
  task_runner_->PostDelayedTask(FROM_HERE, TimeDelta(10000), std::bind(&FlutterVideoRendererSink::RenderTask, this));
}

} // namespace media