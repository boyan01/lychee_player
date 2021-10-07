//
// Created by yangbin on 2021/5/16.
//

#ifndef MEDIA_FLUTTER_EXTERNAL_VIDEO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_EXTERNAL_VIDEO_RENDERER_SINK_H_

#include "video_renderer_sink.h"
#include "external_media_texture.h"

#include "base/task_runner.h"

extern "C" {
#include "libswscale/swscale.h"
}

namespace media {

class ExternalVideoRendererSink : public VideoRendererSink {

 public:

  static FlutterTextureAdapterFactory factory_;

  static AVPixelFormat GetPixelFormat(ExternalMediaTexture::PixelFormat format);

  ExternalVideoRendererSink();

  explicit ExternalVideoRendererSink(const TaskRunner& task_runner);

  ~ExternalVideoRendererSink() override;

  int64_t texture_id() { return texture_->GetTextureId(); };

  void Start(RenderCallback *callback) override;

  void Stop() override;

 private:

  std::unique_ptr<ExternalMediaTexture> texture_;

  struct SwsContext *img_convert_ctx_ = nullptr;

  void OnTextureAvailable(std::unique_ptr<ExternalMediaTexture> texture);

  bool destroyed_;

  enum State { kIdle, kRunning };
  State state_ = kIdle;

  TaskRunner task_runner_;
  RenderCallback *render_callback_;

  std::mutex render_mutex_;

  void RenderTask();

  void DoRender(const std::shared_ptr<VideoFrame> &frame);

};

}

#endif //MEDIA_FLUTTER_EXTERNAL_VIDEO_RENDERER_SINK_H_
