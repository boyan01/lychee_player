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

VideoRendererSinkImpl::VideoRendererSinkImpl() {
  DCHECK(factory_) << "factory_ do not register yet.";
  // FIXME bind weak.
  factory_(std::bind(&VideoRendererSinkImpl::OnTextureAvailable, this, std::placeholders::_1));
}

VideoRendererSinkImpl::~VideoRendererSinkImpl() {
  sws_freeContext(img_convert_ctx_);
  if (texture_) {
    texture_->Release();
  }
}

int64_t VideoRendererSinkImpl::Attach() {
  if (!texture_) {
    return -1;
  }
  return texture_->GetTextureId();
}

void VideoRendererSinkImpl::Detach() {

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

void VideoRendererSinkImpl::OnTextureAvailable(std::unique_ptr<FlutterMediaTexture> texture) {
  DLOG_IF(WARNING, !texture) << "register texture failed!";
  texture_ = std::move(texture);
}

} // namespace media