//
// Created by yangbin on 2021/5/16.
//

#include "video_renderer_sink_impl.h"

#include "base/logging.h"

namespace media {

FlutterTextureAdapterFactory VideoRendererSinkImpl::factory_ = nullptr;

VideoRendererSinkImpl::VideoRendererSinkImpl() = default;

int64_t VideoRendererSinkImpl::Attach() {
  DCHECK(!texture_);
  texture_ = factory_();
  DCHECK(texture_);
  return texture_->GetTextureId();
}
void VideoRendererSinkImpl::Detach() {
  DCHECK(texture_);
  texture_->Release();
}

void VideoRendererSinkImpl::DoRender(std::shared_ptr<VideoFrame> frame) {
  if (!texture_ || frame->IsEmpty()) {
    return;
  }
  DCHECK(frame);
  auto *av_frame = frame->frame();
  DCHECK(av_frame);
  texture_->Render(av_frame->data, av_frame->linesize, av_frame->width, av_frame->height);
}

} // namespace media