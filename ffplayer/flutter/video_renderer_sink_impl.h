//
// Created by yangbin on 2021/5/16.
//

#ifndef MEDIA_FLUTTER_VIDEO_RENDERER_SINK_IMPL_H_
#define MEDIA_FLUTTER_VIDEO_RENDERER_SINK_IMPL_H_

#include "flutter_video_renderer_sink.h"

#include "media_api.h"

namespace media {

class VideoRendererSinkImpl : public FlutterVideoRendererSink {

 public:

  VideoRendererSinkImpl();

  static FlutterTextureAdapterFactory factory_;

  int64_t Attach() override;

  void Detach() override;

 protected:

  void DoRender(std::shared_ptr<VideoFrame> frame) override;

 private:

  std::unique_ptr<FlutterMediaTexture> texture_;

};

}

#endif //MEDIA_FLUTTER_VIDEO_RENDERER_SINK_IMPL_H_
