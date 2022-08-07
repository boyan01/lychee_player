//
// Created by boyan on 2021/2/16.
//

#include <cmath>

#include "render_video_base.h"
#include "ffp_utils.h"

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

VideoRenderBase::VideoRenderBase() {
  picture_queue = std::make_unique<FrameQueue>();
}

void VideoRenderBase::Init(const std::shared_ptr<PacketQueue> &video_queue,
                           std::shared_ptr<MediaClock> clock_ctx,
                           std::shared_ptr<MessageContext> msg_ctx) {
  clock_context = std::move(clock_ctx);
  msg_ctx_ = std::move(msg_ctx);
  picture_queue->Init(video_queue.get(), VIDEO_PICTURE_QUEUE_SIZE, 1);
}

VideoRenderBase::~VideoRenderBase() = default;

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

int VideoRenderBase::PushFrame(AVFrame *src_frame, double pts, double duration, int pkt_serial) {
  // check video frame pts if needed.
  if (framedrop > 0 || (framedrop && clock_context->GetMasterSyncType() != AV_SYNC_VIDEO_MASTER)) {
    if (src_frame->pts != AV_NOPTS_VALUE) {
      double diff = pts - clock_context->GetMasterClock();
      if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD && diff < 0 &&
          pkt_serial == clock_context->GetVideoClock()->serial &&
          picture_queue->pktq->nb_packets) {
        frame_drop_count_pre++;
        return 0;
      }
    }
  }

//  av_log(nullptr, AV_LOG_INFO, "VideoRenderBase: push frame pts= %0.3f , serial = %d\n", pts, pkt_serial);

  auto *vp = picture_queue->PeekWritable();
  if (!vp) {
    return -1;
  }
#if defined(DEBUG_SYNC)
  printf("frame_type=%c pts=%0.3f\n",
         av_get_picture_type_char(src_frame->pict_type), pts);
#endif

  vp->sar = src_frame->sample_aspect_ratio;
  vp->uploaded = 0;

  vp->width = src_frame->width;
  vp->height = src_frame->height;
  vp->format = src_frame->format;

  vp->pts = pts;
  vp->duration = duration;
  vp->pos = src_frame->pkt_pos;
  vp->serial = pkt_serial;

  if (!first_video_frame_loaded) {
    first_video_frame_loaded = true;
    msg_ctx_->NotifyMsg(FFP_MSG_VIDEO_FRAME_LOADED, vp->width, vp->height);
  }

  av_frame_move_ref(vp->frame, src_frame);
  picture_queue->Push();
  return 0;
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
  bool realtime = false; // TODO check is realtime
  if (!paused_ && clock_context->GetMasterSyncType() == AV_SYNC_EXTERNAL_CLOCK && realtime) {
//        check_external_clock_speed
  }
#endif

  retry:
  if (picture_queue->NbRemaining() == 0) {
    // nothing to do, no picture to display in the queue
  } else {
    double last_duration, duration, delay, time;
    auto last_vp = picture_queue->PeekLast();
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
        msg_ctx_->NotifyMsg(FFP_MSG_VIDEO_RENDERING_START, vp->width, vp->height);
      }
      RenderPicture(*vp);
    }
  }

  NotifyRenderProceed();

  force_refresh_ = false;
  return remaining_time;
}

void VideoRenderBase::Abort() {
  picture_queue->Signal();
}

bool VideoRenderBase::IsReady() {
  return picture_queue->NbRemaining() > 0;
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
  av_log(nullptr, AV_LOG_INFO, "video_render, frame: %d/%d.\n", picture_queue->NbRemaining(), picture_queue->max_size);
}

static void check_external_clock_speed() {
//    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
//        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
//        is->extclk.SetSpeed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.GetSpeed() - EXTERNAL_CLOCK_SPEED_STEP));
//    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
//               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
//        is->extclk.SetSpeed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.GetSpeed() + EXTERNAL_CLOCK_SPEED_STEP));
//    } else {
//        double speed = is->extclk.GetSpeed();
//        if (speed != 1.0) {
//            is->extclk.SetSpeed(speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
//        }
//    }
}
