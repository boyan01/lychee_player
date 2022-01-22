//
// Created by yangbin on 2021/5/5.
//
#include "video_renderer.h"

#include "base/bind_to_current_loop.h"
#include "base/lambda.h"
#include "base/logging.h"
#include "cmath"

namespace {
// 默认绘制下一帧的延迟时延。
// TODO: 根据过去播放的平均帧率进行计算？
const media::TimeDelta kDefaultVideoRenderDelay =
    media::TimeDelta::FromMilliseconds(10);
}  // namespace

namespace media {

VideoRenderer::VideoRenderer(
    const TaskRunner &media_task_runner,
    std::shared_ptr<TaskRunner> decode_task_runner,
    std::shared_ptr<VideoRendererSink> video_renderer_sink)
    : decode_task_runner_(std::move(decode_task_runner)),
      media_task_runner_(media_task_runner),
      sink_(std::move(video_renderer_sink)),
      ready_frames_() {
  DCHECK(decode_task_runner_);
  DCHECK(sink_);
  DCHECK(media_task_runner_);
}

VideoRenderer::~VideoRenderer() { sink_->Stop(); }

void VideoRenderer::Initialize(DemuxerStream *stream,
                               std::shared_ptr<MediaClock> media_clock,
                               VideoRenderer::InitCallback init_callback) {
  DCHECK(media_task_runner_.BelongsToCurrentThread());
  DCHECK(stream);
  DCHECK(media_clock);
  DCHECK(init_callback);
  DCHECK_EQ(state_, kUnInitialized);
  state_ = kInitializing;
  media_clock_ = std::move(media_clock);
  init_callback_ = std::move(BindToCurrentLoop(std::move(init_callback)));
  decoder_stream_ = std::make_shared<VideoDecoderStream>(
      std::make_unique<DecoderStreamTraits<DemuxerStream::Video>>(),
      decode_task_runner_);
  decoder_stream_->Initialize(
      stream,
      bind_weak(&VideoRenderer::OnDecodeStreamInitialized, shared_from_this()));
}

void VideoRenderer::OnDecodeStreamInitialized(bool success) {
  LOG_IF(WARNING, !success) << "Failed to init video decoder stream.";
  state_ = kFlushed;
  init_callback_(success);
  init_callback_ = nullptr;
  if (!success) {
    return;
  }
  decode_task_runner_->PostTask(
      FROM_HERE,
      bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
}

void VideoRenderer::AttemptReadFrame() {
  DCHECK(decode_task_runner_->BelongsToCurrentThread());
  DCHECK(decoder_stream_);

  if (!CanDecodeMore() || reading_) {
    return;
  }
  reading_ = true;
  decoder_stream_->Read(
      bind_weak(&VideoRenderer::OnNewFrameAvailable, shared_from_this()));
}

void VideoRenderer::OnNewFrameAvailable(std::shared_ptr<VideoFrame> frame) {
  DLOG_IF(WARNING, ready_frames_.size() > 3)
      << "ready_frames is enough. " << ready_frames_.size();
  reading_ = false;
  ready_frames_.emplace_back(std::move(frame));

  decode_task_runner_->PostTask(
      FROM_HERE,
      bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
}

bool VideoRenderer::CanDecodeMore() { return ready_frames_.size() < 3; }

void VideoRenderer::Start() {
  DCHECK(media_task_runner_.BelongsToCurrentThread());
  DCHECK_NE(state_, kUnInitialized);

  sink_->Start(this);
  state_ = kPlaying;
}
void VideoRenderer::Stop() {
  DCHECK(media_task_runner_.BelongsToCurrentThread());
  sink_->Stop();
  state_ = kFlushed;
}

std::shared_ptr<VideoFrame> VideoRenderer::Render(TimeDelta &next_frame_delay) {
  next_frame_delay = kDefaultVideoRenderDelay;
  TRACE_METHOD_DURATION(2);

  if (state_ != kPlaying) {
    DLOG(WARNING) << "not playing: " << state_;
    return VideoFrame::CreateEmptyFrame();
  }

  if (ready_frames_.empty()) {
    decode_task_runner_->PostTask(
        FROM_HERE,
        bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));
    return VideoFrame::CreateEmptyFrame();
  }

  double clock = GetDrawingClock();
  if (std::isnan(clock)) {
    return VideoFrame::CreateEmptyFrame();
  }

  auto last_frame = ready_frames_.front();
  // 当前如果还有有更多可用的 frame 可用。
  // 那么再 check 一下后面的 frame 是否更加适合当前的时间。
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
      if (!ready_frames_.empty()) {
        const std::shared_ptr<VideoFrame> &next_frame = ready_frames_.front();
        next_frame_delay = TimeDelta::FromSecondsD(
            std::min(next_frame->pts() - clock,
                     clock - current_frame->pts() + current_frame->duration()));
        //        DCHECK_GT(next_frame_delay, TimeDelta::FromMicroseconds(-1));
        if (next_frame_delay < TimeDelta()) {
          next_frame_delay = TimeDelta();
        }
      }
      ready_frames_.push_front(current_frame);
    }
  }

  auto frame = ready_frames_.front();
  DCHECK(frame);

  decode_task_runner_->PostTask(
      FROM_HERE,
      bind_weak(&VideoRenderer::AttemptReadFrame, shared_from_this()));

  return frame;
}

void VideoRenderer::OnFrameDrop() {}

double VideoRenderer::GetDrawingClock() {
  DCHECK(media_clock_);
  return media_clock_->GetMasterClock();
}

void VideoRenderer::Flush() {
  DCHECK(media_task_runner_.BelongsToCurrentThread());
  if (state_ == kFlushing || state_ == kUnInitialized) {
    DLOG(INFO) << "abort video renderer flush, current state: " << state_;
    return;
  }
  bool playing = state_ == kPlaying;
  state_ = kFlushing;
  decoder_stream_->Flush();
  ready_frames_.clear();
  state_ = playing ? kPlaying : kFlushed;
}

std::ostream &operator<<(std::ostream &os, const VideoRenderer &renderer) {
  os << " state_: " << renderer.state_
     << " ready_frames_: " << renderer.ready_frames_.size()
     << " frame_drop_count_: " << renderer.frame_drop_count_
     << " reading_: " << renderer.reading_;
  return os;
}

}  // namespace media