//
// Created by yangbin on 2021/5/5.
//
#include "base/logging.h"

#include "video_renderer.h"

#include "cmath"

#include "base/bind_to_current_loop.h"
#include "base/lambda.h"

namespace media {

VideoRenderer::VideoRenderer(
    std::shared_ptr<TaskRunner> task_runner,
    std::shared_ptr<VideoRendererSink> video_renderer_sink
) : task_runner_(std::move(task_runner)),
    sink_(std::move(video_renderer_sink)),
    ready_frames_() {
  DCHECK(task_runner_);
  DCHECK(sink_);
}

VideoRenderer::~VideoRenderer() {
  sink_->Stop();
}

void VideoRenderer::Initialize(DemuxerStream *stream, std::shared_ptr<MediaClock> media_clock,
                               VideoRenderer::InitCallback init_callback) {

  DCHECK(stream);
  DCHECK(media_clock);
  DCHECK(init_callback);
  DCHECK_EQ(state_, kUnInitialized);

  state_ = kInitializing;
  media_clock_ = std::move(media_clock);
  init_callback_ = std::move(BindToCurrentLoop(std::move(init_callback)));
  decoder_stream_ = std::make_shared<VideoDecoderStream>(
      std::make_unique<DecoderStreamTraits<DemuxerStream::Video>>(),
      task_runner_
  );
  decoder_stream_->Initialize(stream, bind_weak(&VideoRenderer::OnDecodeStreamInitialized, shared_from_this()));

}

void VideoRenderer::OnDecodeStreamInitialized(bool success) {
  LOG_IF(WARNING, !success) << "Failed to init video decoder stream.";
  state_ = kFlushed;
  init_callback_(success);
  init_callback_ = nullptr;
  if (!success) {
    return;
  }
  task_runner_->PostTask(FROM_HERE, bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
}

void VideoRenderer::AttemptReadFrame() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(decoder_stream_);

  if (!CanDecodeMore() || reading_) {
    return;
  }
  reading_ = true;
  decoder_stream_->Read(bind_weak(&VideoRenderer::OnNewFrameAvailable, shared_from_this()));
}

void VideoRenderer::OnNewFrameAvailable(std::shared_ptr<VideoFrame> frame) {
  DLOG_IF(WARNING, ready_frames_.size() > 3) << "ready_frames is enough. " << ready_frames_.size();
  reading_ = false;
  ready_frames_.emplace_back(std::move(frame));

  task_runner_->PostTask(FROM_HERE, bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
}

bool VideoRenderer::CanDecodeMore() {
  return ready_frames_.size() < 3;
}

void VideoRenderer::Start() {
  DCHECK_NE(state_, kUnInitialized);

  sink_->Start(this);
}
void VideoRenderer::Stop() {
  sink_->Stop();
}

std::shared_ptr<VideoFrame> VideoRenderer::Render() {

  if (ready_frames_.empty()) {
    task_runner_->PostTask(FROM_HERE, bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
    return VideoFrame::CreateEmptyFrame();
  }

  auto last_frame = ready_frames_.front();
  double clock = GetDrawingClock();
  if (std::isnan(clock)) {
    return VideoFrame::CreateEmptyFrame();
  }

  if (last_frame->pts() > clock) {
    // It's not time to display next frame. still display current frame again.
//    remaining_time = std::min(clock - last_frame->pts(), remaining_time);
  } else if (ready_frames_.size() > 1) {
    ready_frames_.pop_front();
    if (!ready_frames_.empty()) {
      auto current_frame = ready_frames_.front();
      ready_frames_.pop_front();
      while (!ready_frames_.empty()) {
        auto next_frame = ready_frames_.front();
        if (next_frame->pts() > clock) {
          break;
        }
        ready_frames_.pop_front();
        frame_drop_count_++;
        current_frame = next_frame;
      }
      ready_frames_.push_front(current_frame);
    }
  }

  auto frame = ready_frames_.front();
  DCHECK(frame);

  task_runner_->PostTask(FROM_HERE, bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));

  return frame;
}

void VideoRenderer::OnFrameDrop() {

}

double VideoRenderer::GetDrawingClock() {
  DCHECK(media_clock_);
  return media_clock_->GetMasterClock();
}

void VideoRenderer::Flush() {
  task_runner_->PostTask(FROM_HERE, [&]() {
    decoder_stream_->Flush();
    ready_frames_.clear();
  });
}

std::ostream &operator<<(std::ostream &os, const VideoRenderer &renderer) {
  os << " state_: " << renderer.state_
     << " ready_frames_: " << renderer.ready_frames_.size()
     << " frame_drop_count_: " << renderer.frame_drop_count_
     << " reading_: " << renderer.reading_;
  return os;
}

}