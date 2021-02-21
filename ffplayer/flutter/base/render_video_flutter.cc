//
// Created by boyan on 2021/2/16.
//

#include "render_video_flutter.h"
#include "ffp_utils.h"
#include "ffp_define.h"

void FlutterVideoRender::StartRenderThread() {
  CHECK_VALUE(!render_started_);
  render_started_ = true;
  abort_render_ = false;
  render_mutex_ = new std::mutex;
  thread_ = new std::thread(&FlutterVideoRender::RenderThread, this);
}

void FlutterVideoRender::RenderThread() {
  update_thread_name("video_render");

  double remaining_time = 0.0;
  while (!abort_render_) {
    if (remaining_time > 0.0)
      av_usleep((int64_t) (remaining_time * 1000000.0));
    if (abort_render_) {
      break;
    }
    remaining_time = DrawFrame();
  }

  av_log(nullptr, AV_LOG_INFO, "flutter_video_render_thread done.\n");
}
void FlutterVideoRender::Abort() {
  VideoRenderBase::Abort();
  StopRenderThread();
}

void FlutterVideoRender::StopRenderThread() {
  CHECK_VALUE(render_started_);
  abort_render_ = true;
  picture_queue->Signal();
  if (thread_->joinable()) {
    thread_->join();
  }
  delete render_mutex_;
  delete thread_;
  render_started_ = false;
}
