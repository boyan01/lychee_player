//
// Created by yangbin on 2021/5/16.
//

#include "video_renderer_sink_impl.h"

#include "base/logging.h"

namespace media {

// static
FlutterTextureAdapterFactory VideoRendererSinkImpl::factory_ = nullptr;

// static
AVPixelFormat VideoRendererSinkImpl::GetPixelFormat(FlutterMediaTexture::PixelFormat format) {
  switch (format) {
    case FlutterMediaTexture::kFormat_32_ARGB: return AV_PIX_FMT_ARGB;
    case FlutterMediaTexture::kFormat_32_BGRA: return AV_PIX_FMT_BGRA;
    case FlutterMediaTexture::kFormat_32_RGBA: return AV_PIX_FMT_RGBA;
  }
  NOTREACHED();
  return AV_PIX_FMT_BGRA;
}

VideoRendererSinkImpl::VideoRendererSinkImpl()
    : attached_count_(0),
      destroyed_(false),
      looper_(base::MessageLooper::PrepareLooper("video_render")),
      task_runner_(std::make_unique<TaskRunner>(looper_)),
      render_callback_(nullptr) {
  DCHECK(factory_) << "factory_ do not register yet.";
  // FIXME bind weak.
  factory_(std::bind(&VideoRendererSinkImpl::OnTextureAvailable, this, std::placeholders::_1));
}

void VideoRendererSinkImpl::Start(VideoRendererSink::RenderCallback *callback) {
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  DCHECK_EQ(state_, kIdle);
  render_callback_ = callback;
  state_ = kRunning;
  task_runner_->PostTask(FROM_HERE, std::bind(&VideoRendererSinkImpl::RenderTask, this));
}

void VideoRendererSinkImpl::Stop() {
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  render_callback_ = nullptr;
  state_ = kIdle;
  task_runner_->RemoveAllTasks();
}

VideoRendererSinkImpl::~VideoRendererSinkImpl() {
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  task_runner_.reset(nullptr);
  looper_->Quit();
  delete looper_;
  sws_freeContext(img_convert_ctx_);
  texture_.reset(nullptr);
  destroyed_ = true;
}

int64_t VideoRendererSinkImpl::Attach() {
  if (!texture_) {
    return -1;
  }
  attached_count_++;
  return texture_->GetTextureId();
}

void VideoRendererSinkImpl::Detach() {
  attached_count_--;
}

void VideoRendererSinkImpl::DoRender(std::shared_ptr<VideoFrame> frame) {
  DCHECK(!destroyed_);

  if (attached_count_ <= 0) {
    DLOG(WARNING) << "skip frame due to no display attached.";
    return;
  }
  if (!texture_ || frame->IsEmpty()) {
    return;
  }

  texture_->MaybeInitPixelBuffer(frame->Width(), frame->Height());

  if (!texture_->TryLockBuffer()) {
    DLOG(WARNING) << "failed to lock buffer, skip render this frame.";
    return;
  }

  auto *output = texture_->GetBuffer();
  DCHECK(output);

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

#if 1
  auto *av_frame = frame->frame();
  int linesize[4] = {4 * texture_->GetWidth()};
  uint8_t *bgr_buffer[8] = {static_cast<uint8_t *>(output)};
  sws_scale(img_convert_ctx_, av_frame->data, av_frame->linesize, 0, av_frame->height, bgr_buffer, linesize);
#endif

#if 0
  auto bitmap = reinterpret_cast<uint32_t *>(output);
  for (unsigned long i = 0; i < texture_->GetWidth() * texture_->GetHeight(); i++) {
    bitmap[i] = 0x000000ff;
  }
  DLOG(INFO) << "render bitmap: width = " << texture_->GetWidth() << " height = " << texture_->GetHeight();
#endif

  texture_->UnlockBuffer();

  texture_->NotifyBufferUpdate();

}

void VideoRendererSinkImpl::OnTextureAvailable(std::unique_ptr<FlutterMediaTexture> texture) {
  DLOG_IF(WARNING, !texture) << "register texture failed!";
  texture_ = std::move(texture);
}

void VideoRendererSinkImpl::RenderTask() {
  if (render_callback_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  DCHECK_NE(state_, kIdle);
  auto frame = render_callback_->Render();
  if (!frame->IsEmpty()) {
    DoRender(std::move(frame));
  }
  // schedule next frame after 10 ms.
  task_runner_->PostDelayedTask(FROM_HERE, TimeDelta(10000), std::bind(&VideoRendererSinkImpl::RenderTask, this));
}

} // namespace media