//
// Created by yangbin on 2021/5/16.
//
#include "base/logging.h"

#include "external_video_renderer_sink.h"

namespace media {

// static
FlutterTextureAdapterFactory ExternalVideoRendererSink::factory_ = nullptr;

// static
AVPixelFormat ExternalVideoRendererSink::GetPixelFormat(ExternalMediaTexture::PixelFormat format) {
  switch (format) {
    case ExternalMediaTexture::kFormat_32_ARGB: return AV_PIX_FMT_ARGB;
    case ExternalMediaTexture::kFormat_32_BGRA: return AV_PIX_FMT_BGRA;
    case ExternalMediaTexture::kFormat_32_RGBA: return AV_PIX_FMT_RGBA;
  }
  NOTREACHED();
  return AV_PIX_FMT_BGRA;
}

ExternalVideoRendererSink::ExternalVideoRendererSink()
    : ExternalVideoRendererSink(TaskRunner(base::MessageLooper::PrepareLooper("video_render"))) {
}

ExternalVideoRendererSink::ExternalVideoRendererSink(const TaskRunner &task_runner) :
    task_runner_(task_runner),
    destroyed_(false),
    render_callback_(nullptr) {
  DCHECK(factory_) << "factory_ do not register yet.";
  // FIXME bind weak.
  factory_(std::bind(&ExternalVideoRendererSink::OnTextureAvailable, this, std::placeholders::_1));
}

void ExternalVideoRendererSink::Start(VideoRendererSink::RenderCallback *callback) {
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  DCHECK_EQ(state_, kIdle);
  render_callback_ = callback;
  state_ = kRunning;
  task_runner_.PostTask(FROM_HERE, std::bind(&ExternalVideoRendererSink::RenderTask, this));
}

void ExternalVideoRendererSink::Stop() {
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  render_callback_ = nullptr;
  state_ = kIdle;
  task_runner_.RemoveAllTasks();
}

ExternalVideoRendererSink::~ExternalVideoRendererSink() {
  task_runner_.Reset();
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  sws_freeContext(img_convert_ctx_);
  texture_.reset(nullptr);
  destroyed_ = true;
}

void ExternalVideoRendererSink::DoRender(const std::shared_ptr<VideoFrame> &frame) {
  DCHECK(!destroyed_);

  if (!texture_ || frame->IsEmpty()) {
    return;
  }

  texture_->MaybeInitPixelBuffer(frame->Width(), frame->Height());

  if (!texture_->TryLockBuffer()) {
    DLOG(WARNING) << "failed to lock buffer, skip render this frame.";
    return;
  }

  if (frame->frame()->hw_frames_ctx != nullptr) {
    DLOG(WARNING) << "DoRender with hardware accel";
    texture_->RenderWithHWAccel(frame->frame()->data[3]);
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
    DLOG(ERROR) << "can not init image convert context\n"
                << "texture: " << texture_->GetWidth() << "x" << texture_->GetHeight() << " " << frame->frame()->format
                << "\n"
                << "frame: " << frame->Width() << "x" << frame->Height() << " "
                << GetPixelFormat(texture_->GetSupportFormat()) << "\n";
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

void ExternalVideoRendererSink::OnTextureAvailable(std::unique_ptr<ExternalMediaTexture> texture) {
  DLOG_IF(WARNING, !texture) << "register texture failed!";
  texture_ = std::move(texture);
}

void ExternalVideoRendererSink::RenderTask() {
  if (render_callback_ == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock_guard(render_mutex_);
  DCHECK_NE(state_, kIdle);
  TimeDelta next_delay;
  auto frame = render_callback_->Render(next_delay);
  if (!frame->IsEmpty()) {
    DoRender(frame);
  }
  // schedule next frame after 10 ms.
  task_runner_.PostDelayedTask(FROM_HERE,
                               next_delay,
                               std::bind(&ExternalVideoRendererSink::RenderTask, this));
}

} // namespace media