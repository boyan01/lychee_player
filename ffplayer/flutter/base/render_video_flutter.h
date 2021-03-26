//
// Created by boyan on 2021/2/16.
//

#ifndef AUDIO_PLAYER_EXAMPLE_RENDER_VIDEO_FLUTTER_H
#define AUDIO_PLAYER_EXAMPLE_RENDER_VIDEO_FLUTTER_H

#include <thread>
#include <mutex>

#include "render_video_base.h"

namespace media {

class FlutterVideoRender : public VideoRenderBase {

 private:
  bool abort_render_ = false;
  bool render_started_ = false;
  std::thread *thread_;
  std::mutex *render_mutex_;

 private:

  void RenderThread();

 public:

  /**
   * start VideoRender thread.
   */
  void StartRenderThread();

  void StopRenderThread();

  void Abort() override;

};

}

#endif //AUDIO_PLAYER_EXAMPLE_RENDER_VIDEO_FLUTTER_H
