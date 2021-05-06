//
// Created by yangbin on 2021/5/6.
//

#ifndef MEDIA_FLUTTER_ANDROID_ANDROID_VIDEO_RENDERER_SINK_H_
#define MEDIA_FLUTTER_ANDROID_ANDROID_VIDEO_RENDERER_SINK_H_

extern "C" {
#include "libswscale/swscale.h"
}

#include "flutter_video_renderer_sink.h"
#include "flutter_texture_entry.h"

namespace media {

class AndroidVideoRendererSink : public FlutterVideoRendererSink {

 public:

  typedef std::function<std::unique_ptr<FlutterTextureEntry>()> TextureRegistry;
  static TextureRegistry flutter_texture_registry;

  AndroidVideoRendererSink();

  virtual ~AndroidVideoRendererSink();

  int64_t Attach();

  void Detach();

 protected:

  void DoRender(std::shared_ptr<VideoFrame> frame) override;

 private:

  ANativeWindow_Buffer window_buffer_;
  struct SwsContext *img_convert_ctx_;

  std::unique_ptr<FlutterTextureEntry> texture_;

};

}

#endif //MEDIA_FLUTTER_ANDROID_ANDROID_VIDEO_RENDERER_SINK_H_
