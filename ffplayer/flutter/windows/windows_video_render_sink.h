//
// Created by yangbin on 2021/5/8.
//

#ifndef MEDIA_FLUTTER_WINDOWS_FLUTTER_WINDOWS_VIDEO_RENDER_SINK_H_
#define MEDIA_FLUTTER_WINDOWS_FLUTTER_WINDOWS_VIDEO_RENDER_SINK_H_

#include "flutter/flutter_engine.h"

extern "C" {
#include "libswscale/swscale.h"
}

#include "flutter_video_renderer_sink.h"
#include "ffp_define.h"

namespace media {

class WindowsVideoRenderSink : public FlutterVideoRendererSink {

 public:

  int64_t Attach() override;

  void Detach() override;

 protected:

  void DoRender(std::shared_ptr<VideoFrame> frame) override;

 private:

  int64_t texture_id_ = -1;
  FlutterDesktopPixelBuffer *pixel_buffer_ = nullptr;
  std::unique_ptr<flutter::TextureVariant> texture_;
  struct SwsContext *img_convert_ctx_ = nullptr;

};

}

FFPLAYER_EXPORT void register_flutter_plugin(flutter::TextureRegistrar *texture_registrar);

#endif //MEDIA_FLUTTER_WINDOWS_FLUTTER_WINDOWS_VIDEO_RENDER_SINK_H_
