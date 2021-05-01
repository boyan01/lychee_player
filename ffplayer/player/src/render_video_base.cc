//
// Created by boyan on 2021/2/16.
//

#include <cmath>

#include "base/logging.h"
#include "base/lambda.h"
#include "base/bind_to_current_loop.h"

#include "render_video_base.h"
#include "ffp_utils.h"

namespace media {

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1


/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

VideoRenderBase::VideoRenderBase() : frame_queue_(VIDEO_PICTURE_QUEUE_SIZE) {

}

VideoRenderBase::~VideoRenderBase() = default;

void VideoRenderBase::Initialize(
    VideoRenderHost *host,
    DemuxerStream *demuxer_stream,
    std::shared_ptr<MediaClock> clock_ctx,
    std::shared_ptr<VideoDecoder> decoder) {
  task_runner_ = TaskRunner::current();
  DCHECK(task_runner_);
  DCHECK(host);
  render_host_ = host;
  clock_context = std::move(clock_ctx);
  decoder_ = std::move(decoder);
  task_runner_->PostTask(FROM_HERE, [&]() {
    decoder_->ReadFrame(bind_weak(&VideoRenderBase::OnNewFrameReady, shared_from_this()));
  });
}

void VideoRenderBase::OnNewFrameReady(std::shared_ptr<VideoFrame> frame) {
  if (!frame || frame->IsEmpty()) {
    return;
  }
  DLOG(INFO) << "frame: " << frame->pts();
  frame_queue_.InsertLast(std::move(frame));
  if (frame_queue_.IsFull()) {
    return;
  }
  task_runner_->PostTask(FROM_HERE, [&]() {
    decoder_->ReadFrame(bind_weak(&VideoRenderBase::OnNewFrameReady, shared_from_this()));
  });
}

double VideoRenderBase::VideoPictureDuration(Frame *vp, Frame *next_vp) const {
  if (vp->serial == next_vp->serial) {
    double duration = next_vp->pts - vp->pts;
    if (isnan(duration) || duration <= 0 || duration > max_frame_duration) {
      return vp->duration;
    } else {
      return duration;
    }
  } else {
    return 0;
  }
}

double VideoRenderBase::ComputeTargetDelay(double delay) const {
  double sync_threshold, diff = 0;

  /* update delay to follow master synchronisation source */
  if (clock_context->GetMasterSyncType() != AV_SYNC_VIDEO_MASTER) {
    /* if video is not main clock, we try to correct big delays by duplicating or deleting a frame */
    // diff > 0, video need wait audio.
    // diff < 0, video need skip frame.
    diff = clock_context->GetVideoClock()->GetClock() - clock_context->GetMasterClock();

    /* skip or repeat frame. We take into account the delay to compute the threshold. I still don't know if it is the best guess */
    sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
    if (!std::isnan(diff) && fabs(diff) < max_frame_duration) {
      if (diff <= -sync_threshold) {
        // try to render next frame as soon.
        delay = FFMAX(0, delay + diff);
      } else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
        delay = delay + diff;
      } else if (diff >= sync_threshold) {
        // Double current frame present duration. Waiting next frame.
        delay = 2 * delay;
      }
    }
  }

  av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);
  return delay;
}

double VideoRenderBase::GetVideoAspectRatio() const {
  if (!first_video_frame_loaded) {
    return 0;
  }
  if (frame_height == 0) {
    return 0;
  }
  return ((double) frame_width) / frame_height;
}

double VideoRenderBase::DrawFrame() {
  double remaining_time = REFRESH_RATE;

  if (paused_ && !force_refresh_) {
    NotifyRenderProceed();
    return remaining_time;
  }

#if 0
  retry:
  if (frame_queue_.IsEmpty()) {
    // nothing to do, no picture to display in the queue
  } else {
    double last_duration, duration, delay, time;
    auto last_vp = frame_queue_->PeekLast();
    auto vp = picture_queue->Peek();
    if (vp->serial != picture_queue->queue->serial) {
      picture_queue->Next();
      goto retry;
    }

    if (last_vp->serial != vp->serial) {
      frame_timer = get_relative_time();
    }

    if (paused_) {
      goto display;
    }

    /* compute nominal last_duration */
    last_duration = VideoPictureDuration(last_vp, vp);
    delay = ComputeTargetDelay(last_duration);

    time = get_relative_time();
    if (time < frame_timer + delay) {
      // It's not time to display current frame. still display last frame again.
      remaining_time = FFMIN(frame_timer + delay - time, remaining_time);
      goto display;
    }

    frame_timer += delay;
    if (delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX) {
      frame_timer = time;
    }

    {
      std::lock_guard<std::recursive_mutex> lock(picture_queue->mutex);
      if (!isnan(vp->pts)) {
        // update_video_pts
        clock_context->GetVideoClock()->SetClock(vp->pts, vp->serial);
        clock_context->GetExtClock()->Sync(clock_context->GetVideoClock());
      }
    }

    if (picture_queue->NbRemaining() > 1) {
      auto *next_vp = picture_queue->PeekNext();
      duration = VideoPictureDuration(vp, next_vp);
      if (!step && ShouldDropFrames() && time > frame_timer + duration) {
        // Next frame is the best candidate, drop current frame.
        frame_drop_count++;
        picture_queue->Next();
        goto retry;
      }
    }

    picture_queue->Next();
    force_refresh_ = true;
    if (step && !paused_) {
//            ffplayer_toggle_pause()
    }
  }
  display:
  if (force_refresh_ && picture_queue->rindex_shown) {
    auto *vp = picture_queue->PeekLast();
    if (vp) {
      if (first_video_frame_rendered) {
        first_video_frame_rendered = true;
        render_host_->OnFirstFrameRendered(vp->width, vp->height);
      }
      RenderPicture(*vp);
    }
  }

  NotifyRenderProceed();

#endif

  if (frame_queue_.IsEmpty()) {
    return remaining_time;
  }

  auto last_frame = frame_queue_.GetFront();
  double clock = GetDrawingClock();
  if (std::isnan(clock)) {
    return remaining_time;
  }

  if (last_frame->pts() > clock) {
    // It's not time to display next frame. still display current frame again.
    remaining_time = std::min(clock - last_frame->pts(), remaining_time);
  } else if (frame_queue_.GetSize() > 1) {
    frame_queue_.DeleteFront();
    if (!frame_queue_.IsEmpty()) {
      auto current_frame = frame_queue_.PopFront();
      while (!frame_queue_.IsEmpty()) {
        auto next_frame = frame_queue_.GetFront();
        if (next_frame->pts() > clock) {
          break;
        }
        frame_queue_.DeleteFront();
        frame_drop_count++;
        current_frame = next_frame;
      }
      frame_queue_.InsertFront(current_frame);
    }
    force_refresh_ = true;
    frame_timer_ = get_relative_time();
  }

  auto frame = frame_queue_.GetFront();
  DCHECK(frame);

  if (force_refresh_) {
    if (!first_video_frame_rendered) {
      first_video_frame_rendered = true;
      render_host_->OnFirstFrameRendered(frame->Width(), frame->Height());
    }
    RenderPicture(frame);
  }

  force_refresh_ = false;

  if (!frame_queue_.IsFull()) {
    task_runner_->PostTask(FROM_HERE, [&]() {
      decoder_->ReadFrame(bind_weak(&VideoRenderBase::OnNewFrameReady, shared_from_this()));
    });
  }

  return remaining_time;

}

double VideoRenderBase::GetDrawingClock() {
  DCHECK(clock_context);
  return clock_context->GetMasterClock();
}

void VideoRenderBase::Abort() {
  // TODO
}

bool VideoRenderBase::IsReady() {
  // TODO
  return true;
}

bool VideoRenderBase::ShouldDropFrames() const {
  return framedrop > 0 || framedrop > 0 && clock_context->GetMasterSyncType() != AV_SYNC_VIDEO_MASTER;
}

void VideoRenderBase::Start() {
  paused_ = false;
}

void VideoRenderBase::Stop() {
  paused_ = true;
}

void VideoRenderBase::DumpDebugInformation() {
  // TODO
}

void VideoRenderBase::Flush() {
  task_runner_->PostTask(FROM_HERE, [&]() {
    DLOG(INFO) << "on flush";
    frame_queue_.Clear();
    decoder_->ReadFrame(bind_weak(&VideoRenderBase::OnNewFrameReady, shared_from_this()));
  });
}

static void check_external_clock_speed() {

}

}