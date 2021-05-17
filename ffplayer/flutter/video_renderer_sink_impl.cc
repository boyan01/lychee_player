//
// Created by yangbin on 2021/5/16.
//

#include "video_renderer_sink_impl.h"

#include "base/logging.h"

namespace media {

FlutterTextureAdapterFactory VideoRendererSinkImpl::factory_ = nullptr;

AVPixelFormat VideoRendererSinkImpl::GetPixelFormat(FlutterMediaTexture::PixelFormat format) {
  return AV_PIX_FMT_BGRA;
}

VideoRendererSinkImpl::VideoRendererSinkImpl() = default;

VideoRendererSinkImpl::~VideoRendererSinkImpl() {
  sws_freeContext(img_convert_ctx_);
}

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

  texture_->MaybeInitPixelBuffer(frame->Width(), frame->Height());

  texture_->LockBuffer();

  auto *output = texture_->GetBuffer();
  if (!output) {
    DLOG(WARNING) << "texture buffer is empty.";
    texture_->UnlockBuffer();
    return;
  }

  img_convert_ctx_ = sws_getCachedContext(
      img_convert_ctx_, frame->Width(), frame->Height(), AVPixelFormat(frame->frame()->format),
      texture_->GetWidth(), texture_->GetHeight(),
      GetPixelFormat(texture_->GetSupportFormat()), SWS_BICUBIC,
      nullptr, nullptr, nullptr);

  if (!img_convert_ctx_) {
    DLOG(ERROR) << "can not init image convert context";
    texture_->UnlockBuffer();
    return;
  }

  auto *av_frame = frame->frame();
  int linesize[4] = {4 * texture_->GetWidth()};
  uint8_t *bgr_buffer[8] = {static_cast<uint8_t *>(output)};
  sws_scale(img_convert_ctx_, av_frame->data, av_frame->linesize, 0, av_frame->height, bgr_buffer, linesize);

  texture_->UnlockBuffer();

  texture_->NotifyBufferUpdate();

}

} // namespace media