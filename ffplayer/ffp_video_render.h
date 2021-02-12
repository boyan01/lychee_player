//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_VIDEO_RENDER_H
#define FFPLAYER_FFP_VIDEO_RENDER_H

#include <thread>
#include <mutex>
#include <functional>

#include "ffp_clock.h"
#include "ffp_frame_queue.h"
#include "ffplayer/proto.h"

struct VideoRender;

struct FFP_VideoRenderCallback {
    void *opacity = nullptr;

    /**
     *
     * called by video render thread.
     *
     * @param video_render_ctx
     * @param frame
     */
    std::function<void(VideoRender *, Frame *)> on_render = nullptr;

    void (*on_texture_updated)(VideoRender *video_render_ctx) = nullptr;

};

class VideoRender {
public:
    bool abort_render = false;
    bool render_attached = false;
    bool first_video_frame_loaded = false;
    bool first_video_frame_rendered = false;
    int frame_width = 0;
    int frame_height = 0;
    FFP_VideoRenderCallback *render_callback_ = nullptr;

    int framedrop = -1;

    int frame_drop_count = 0;

    ClockContext *clock_context = nullptr;

    double max_frame_duration;  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

private:
    FrameQueue *picture_queue = nullptr;
    std::thread *render_thread_ = nullptr;
    std::mutex *render_mutex_ = nullptr;

    bool paused_ = false;

    bool force_refresh_ = false;

    bool step = false;

    double frame_timer = 0;

private:

    void VideoRenderThread();

    double VideoPictureDuration(Frame *vp, Frame *next_vp) const;

    double ComputeTargetDelay(double delay);

    void RenderPicture();

public:

    VideoRender();

    ~VideoRender();

    void Init(PacketQueue *video_queue, ClockContext *clock_ctx);

    bool Start(CPlayer *player);

    void Stop(CPlayer *player);

    double DrawFrame();

    int PushFrame(AVFrame *frame, double pts, double duration, int pkt_serial);

};


#endif //FFPLAYER_FFP_VIDEO_RENDER_H
