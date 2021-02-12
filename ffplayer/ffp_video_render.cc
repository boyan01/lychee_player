//
// Created by boyan on 2021/2/10.
//

#include <cmath>

#include "ffp_video_render.h"
#include "ffplayer/ffplayer.h"

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is) {
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = is->vidclk.GetClock();
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = is->audclk.GetClock();
            break;
        default:
            val = is->extclk.GetClock();
            break;
    }
    return val;
}

static double compute_target_delay(double delay, VideoState *is) {
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = is->vidclk.GetClock() - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
           delay, -diff);

    return delay;
}


bool VideoRender::Start(CPlayer *player) {
    abort_render = false;
    render_mutex_ = new(std::nothrow) std::mutex();
    if (!render_mutex_) {
        av_log(nullptr, AV_LOG_ERROR, "can not create video_render mutex.");
        return false;
    }
    render_thread_ = new(std::nothrow) std::thread(&VideoRender::VideoRenderThread, this);
    if (!render_thread_) {
        av_log(nullptr, AV_LOG_ERROR, "can not create video_render thread.");
        delete render_mutex_;
        return false;
    }
    return true;
}

void VideoRender::Stop(CPlayer *player) {
    abort_render = true;
    if (render_thread_ && render_thread_->joinable()) {
        render_thread_->join();
    }
    delete render_thread_;
    delete render_mutex_;
}

/**
 * called to display each frame
 *
 * \return time for next frame should be schudled.
 */
double VideoRender::DrawFrame() {
    std::lock_guard<std::mutex> lock(*render_mutex_);
    double remaining_time = 0;

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
            frame_timer = av_gettime_relative() / 1000000.0;
        }

        if (paused_) {
            goto display;
        }

        /* compute nominal last_duration */
        last_duration = VideoPictureDuration(last_vp, vp);
        delay = ComputeTargetDelay(last_duration);

        time = av_gettime_relative() / 1000000.0;
        if (time < frame_timer + delay) {
            remaining_time = FFMIN(frame_timer + delay - time, remaining_time);
            goto display;
        }

        frame_timer += delay;
        if (delay > 0 && time - frame_timer > AV_SYNC_THRESHOLD_MAX) {
            frame_timer = time;
        }

        SDL_LockMutex(picture_queue->mutex);
        if (!isnan(vp->pts)) {
            // update_video_pts
            clock_context->GetVideoClock()->SetClock(vp->pts, vp->serial);
            clock_context->GetExtClock()->Sync(clock_context->GetVideoClock());
        }
        SDL_UnlockMutex(picture_queue->mutex);

        if (picture_queue->NbRemaining() > 1) {
            auto *next_vp = picture_queue->PeekNext();
            duration = VideoPictureDuration(vp, next_vp);
            if (!step && (framedrop > 0 || (framedrop && clock_context->GetMasterSyncType() != AV_SYNC_VIDEO_MASTER))
                && time > frame_timer + duration) {
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
//            video_display(player);
        RenderPicture();
    }

    force_refresh_ = false;
//        show_status(player);
    return remaining_time;
}

int VideoRender::PushFrame(AVFrame *src_frame, double pts, double duration, int pkt_serial) {
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
        // https://forum.videohelp.com/threads/323530-please-explain-SAR-DAR-PAR#post2003533
        frame_width = vp->width;
        frame_height = av_rescale(vp->width, vp->height * vp->sar.den, vp->width * vp->sar.num);
        // TODO send msg2
//        ffp_send_msg2(player, FFP_MSG_VIDEO_FRAME_LOADED, render_ctx->frame_width, render_ctx->frame_height);
    }

    av_frame_move_ref(vp->frame, src_frame);
    picture_queue->Push();
    return 0;
}

VideoRender::VideoRender() {
    picture_queue = new FrameQueue;
}

VideoRender::~VideoRender() {
    delete picture_queue;
}

void VideoRender::Init(PacketQueue *video_queue, ClockContext *clock_ctx) {
    clock_context = clock_ctx;
    picture_queue->Init(video_queue, VIDEO_PICTURE_QUEUE_SIZE, 1);
}

void VideoRender::VideoRenderThread() {
    av_log(nullptr, AV_LOG_INFO, "video_render start\n");
    double remaining_time = 0.0;
    while (!abort_render) {
        if (remaining_time > 0.0)
            av_usleep((int64_t) (remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if ((!paused_ || force_refresh_)) {
            remaining_time = DrawFrame();
        }
    }
}

double VideoRender::VideoPictureDuration(Frame *vp, Frame *next_vp) const {
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

double VideoRender::ComputeTargetDelay(double delay) {
    double sync_threashold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (clock_context->GetMasterSyncType() != AV_SYNC_VIDEO_MASTER) {
        /* if video is not main clock, we try to correct big delays by duplicating or deleting a frame */
        diff = clock_context->GetVideoClock()->GetClock() - clock_context->GetMasterClock();

        /* skip or repeat frame. We take into account the delay to compute the threshold. I still don't know if it is the best guess */
        sync_threashold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!std::isnan(diff) && fabs(diff) < max_frame_duration) {
            if (diff <= -sync_threashold) {
                delay = FFMAX(0, delay + diff);
            } else if (diff >= sync_threashold && delay > AV_SYNC_FRAMEDUP_THRESHOLD) {
                delay = delay + diff;
            } else if (diff >= sync_threashold) {
                delay = 2 * delay;
            }
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n", delay, -diff);

    return 0;
}

void VideoRender::RenderPicture() {
    Frame *vp = picture_queue->PeekLast();
    if (!vp) {
        return;
    }
    if (render_callback_ && render_callback_->on_render) {
        render_callback_->on_render(this, vp);
    }

    if (first_video_frame_rendered) {
        first_video_frame_rendered = true;
//        ffp_send_msg2(player, FFP_MSG_VIDEO_RENDERING_START, vp->width, vp->height);
    }

    if (render_callback_ && render_callback_->on_texture_updated) {
        render_callback_->on_texture_updated(this);
    }
}

static void check_external_clock_speed(VideoState *is) {
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        is->extclk.SetSpeed(FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.GetSpeed() - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
               (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        is->extclk.SetSpeed(FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.GetSpeed() + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.GetSpeed();
        if (speed != 1.0) {
            is->extclk.SetSpeed(speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    is->vidclk.SetClock(pts, serial);
    is->extclk.Sync(&is->vidclk);
}



static void show_status(CPlayer *player) {
    auto is = player->is;
    if (player->show_status) {
        AVBPrint buf;
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st)
                aqsize = is->audioq.size;
            if (is->video_st)
                vqsize = is->videoq.size;
            if (is->subtitle_st)
                sqsize = is->subtitleq.size;
            av_diff = 0;
            if (is->audio_st && is->video_st)
                av_diff = is->audclk.GetClock() - is->vidclk.GetClock();
            else if (is->video_st)
                av_diff = get_master_clock(is) - is->vidclk.GetClock();
            else if (is->audio_st)
                av_diff = get_master_clock(is) - is->audclk.GetClock();

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(&buf,
                       "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%"
                       PRId64
                       "/%"
                       PRId64
                       "   \r",
                       get_master_clock(is),
                       (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A"
                                                                                                      : "   ")),
                       av_diff,
                       is->frame_drops_early + is->frame_drops_late,
                       aqsize / 1024,
                       vqsize / 1024,
                       sqsize,
                       is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                       is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);

            if (player->show_status == 1 && AV_LOG_INFO > av_log_get_level())
                fprintf(stderr, "%s", buf.str);
            else
                av_log(nullptr, AV_LOG_INFO, "%s", buf.str);

            fflush(stderr);
            av_bprint_finalize(&buf, nullptr);

            last_time = cur_time;
        }
    }
}



