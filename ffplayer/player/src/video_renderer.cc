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
    TaskRunner *task_runner,
    std::shared_ptr<VideoRendererSink> video_renderer_sink
) : task_runner_(task_runner),
    sink_(std::move(video_renderer_sink)),
    ready_frames_(3) {
  DCHECK(task_runner_);
  DCHECK(sink_);
}

VideoRenderer::~VideoRenderer() {

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
  DLOG_IF(WARNING, ready_frames_.IsFull()) << "accepted a new frame, but frame pool is full";
  reading_ = false;
  ready_frames_.InsertLast(std::move(frame));

  task_runner_->PostTask(FROM_HERE, bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
}

bool VideoRenderer::CanDecodeMore() {
  return !ready_frames_.IsFull();
}

void VideoRenderer::Start() {
  DCHECK_NE(state_, kUnInitialized);

  sink_->Start(this);
}
void VideoRenderer::Stop() {
  sink_->Stop();
}

std::shared_ptr<VideoFrame> VideoRenderer::Render() {

  if (ready_frames_.IsEmpty()) {
    return VideoFrame::CreateEmptyFrame();
  }

  auto last_frame = ready_frames_.GetFront();
  double clock = GetDrawingClock();
  if (std::isnan(clock)) {
    return VideoFrame::CreateEmptyFrame();
  }

  if (last_frame->pts() > clock) {
    // It's not time to display next frame. still display current frame again.
//    remaining_time = std::min(clock - last_frame->pts(), remaining_time);
  } else if (ready_frames_.GetSize() > 1) {
    ready_frames_.DeleteFront();
    if (!ready_frames_.IsEmpty()) {
      auto current_frame = ready_frames_.PopFront();
      while (!ready_frames_.IsEmpty()) {
        auto next_frame = ready_frames_.GetFront();
        if (next_frame->pts() > clock) {
          break;
        }
        ready_frames_.DeleteFront();
        frame_drop_count_++;
        current_frame = next_frame;
      }
      ready_frames_.InsertFront(current_frame);
    }
  }

  auto frame = ready_frames_.GetFront();
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

}