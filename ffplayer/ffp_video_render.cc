//
// Created by boyan on 2021/2/10.
//

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

static int video_render(CPlayer *player) {
    av_log(nullptr, AV_LOG_INFO, "video_render start\n");
    VideoState *is = player->is;
    FFP_VideoRenderContext *video_render_ctx = &player->video_render_ctx;
    double remaining_time = 0.0;
    while (!video_render_ctx->abort_render) {
        if (remaining_time > 0.0)
            av_usleep((int64_t) (remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh)) {
            video_render_ctx->DrawFrame(player);
        }
    }
    return 0;
}


bool FFP_VideoRenderContext::Start(CPlayer *player) {
    abort_render = false;
    render_mutex_ = new(std::nothrow) std::mutex();
    if (!render_mutex_) {
        av_log(nullptr, AV_LOG_ERROR, "can not create video_render mutex.");
        return false;
    }
    render_thread_ = new(std::nothrow) std::thread(video_render, player);
    if (!render_thread_) {
        av_log(nullptr, AV_LOG_ERROR, "can not create video_render thread.");
        delete render_mutex_;
        return false;
    }
}

void FFP_VideoRenderContext::Stop(CPlayer *player) {
    abort_render = true;
    if (render_thread_ && render_thread_->joinable()) {
        render_thread_->join();
    }
    delete render_thread_;
    delete render_mutex_;
}

static double ffp_draw_frame(CPlayer *player);

double FFP_VideoRenderContext::DrawFrame(CPlayer *player) {
    std::lock_guard<std::mutex> lock(*render_mutex_);
    return ffp_draw_frame(player);
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


static void
video_image_display(CPlayer *player, FrameQueue *pic_queue, FrameQueue *sub_queue) {
    Frame *vp;
    Frame *sp = nullptr;
    FFP_VideoRenderContext *video_render_ctx = &player->video_render_ctx;
    vp = pic_queue->PeekLast();

#if 0
    if (sub_queue) {
        if (frame_queue_nb_remaining(sub_queue) > 0) {
            sp = frame_queue_peek(sub_queue);

            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t *pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(video_render_ctx.renderer, &video_render_ctx.sub_texture,
                                        SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height,
                                        SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        video_render_ctx.sub_convert_ctx = sws_getCachedContext(video_render_ctx.sub_convert_ctx,
                                                                                 sub_rect->w, sub_rect->h,
                                                                                 AV_PIX_FMT_PAL8,
                                                                                 sub_rect->w, sub_rect->h,
                                                                                 AV_PIX_FMT_BGRA,
                                                                                 0, nullptr, nullptr, nullptr);
                        if (!video_render_ctx.sub_convert_ctx) {
                            av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(video_render_ctx.sub_texture, (SDL_Rect *) sub_rect, (void **) pixels,
                                             pitch)) {
                            sws_scale(video_render_ctx.sub_convert_ctx, (const uint8_t *const *) sub_rect->data,
                                      sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(video_render_ctx.sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = nullptr;
        }
    }
#endif

    if (video_render_ctx->render_callback_ && video_render_ctx->render_callback_->on_render) {
        video_render_ctx->render_callback_->on_render(video_render_ctx, vp);
    }

    if (video_render_ctx->first_video_frame_rendered) {
        video_render_ctx->first_video_frame_rendered = true;
        ffp_send_msg2(player, FFP_MSG_VIDEO_RENDERING_START, vp->width, vp->height);
    }

#if 0
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(video_render_ctx.renderer, video_render_ctx.sub_texture, nullptr, &rect);
#else
        int i;
        double xratio = (double) rect.w / (double) sp->width;
        double yratio = (double) rect.h / (double) sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect *) sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                    .y = rect.y + sub_rect->y * yratio,
                    .w = sub_rect->w * xratio,
                    .h = sub_rect->h * yratio};
            SDL_RenderCopy(player->renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
#endif
}


/* display the current picture, if any */
static void video_display(CPlayer *player) {
    VideoState *is = player->is;
    FFP_VideoRenderContext *video_render_ctx = &player->video_render_ctx;
    if (is->video_st) {
        video_image_display(player, &is->pictq, is->subtitle_st ? &is->subpq : nullptr);
    }
    if (video_render_ctx->render_callback_ && video_render_ctx->render_callback_->on_texture_updated) {
        video_render_ctx->render_callback_->on_texture_updated(video_render_ctx);
    }
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
                       (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
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

/**
 * called to display each frame
 *
 * \return time for next frame should be schudled.
 */
static double ffp_draw_frame(CPlayer *player) {
    double remaining_time = 0;
    VideoState *is = player->is;
    double time;

    Frame *sp, *sp2;

    if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
        check_external_clock_speed(is);

    if (!player->video_disable && is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + player->rdftspeed < time) {
            video_display(player);
            is->last_vis_time = time;
        }
        remaining_time = FFMIN(remaining_time, is->last_vis_time + player->rdftspeed - time);
    }

    if (is->video_st) {
        retry:
        if (is->pictq.NbRemaining() == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            Frame *vp, *lastvp;

            /* dequeue the picture */
            lastvp = is->pictq.PeekLast();
            vp = is->pictq.Peek();

            if (vp->serial != is->videoq.serial) {
                is->pictq.Next();
                goto retry;
            }

            if (lastvp->serial != vp->serial)
                is->frame_timer = av_gettime_relative() / 1000000.0;

            if (is->paused)
                goto display;

            /* compute nominal last_duration */
            last_duration = vp_duration(is, lastvp, vp);
            delay = compute_target_delay(last_duration, is);

            time = av_gettime_relative() / 1000000.0;
            if (time < is->frame_timer + delay) {
                remaining_time = FFMIN(is->frame_timer + delay - time, remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                is->frame_timer = time;

            SDL_LockMutex(is->pictq.mutex);
            if (!isnan(vp->pts))
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            SDL_UnlockMutex(is->pictq.mutex);

            if (is->pictq.NbRemaining() > 1) {
                Frame *nextvp = is->pictq.PeekNext();
                duration = vp_duration(is, vp, nextvp);
                if (!is->step && (player->framedrop > 0 ||
                                  (player->framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) &&
                    time > is->frame_timer + duration) {
                    is->frame_drops_late++;
                    is->pictq.Next();
                    goto retry;
                }
            }
#if 0
            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = nullptr;

                    if (sp->serial != is->subtitleq.serial ||
                        (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000))) ||
                        (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000)))) {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (!SDL_LockTexture(player->video_render_ctx.sub_texture, (SDL_Rect *) sub_rect,
                                                     (void **) &pixels,
                                                     &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                        memset(pixels, 0, sub_rect->w << 2);
                                    SDL_UnlockTexture(player->video_render_ctx.sub_texture);
                                }
                            }
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }
#endif
            is->pictq.Next();
            is->force_refresh = 1;

            if (is->step && !is->paused)
                ffplayer_toggle_pause(player);
        }
        display:
        /* display picture */
        if (!player->video_disable && is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
            video_display(player);
    }
    is->force_refresh = 0;
    show_status(player);
    return remaining_time;
}



