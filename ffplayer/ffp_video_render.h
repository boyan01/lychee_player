//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_VIDEO_RENDER_H
#define FFPLAYER_FFP_VIDEO_RENDER_H

#include <thread>
#include <mutex>
#include <functional>
#include <memory>

#include "ffp_clock.h"
#include "ffp_msg_queue.h"
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

    double max_frame_duration = 3600;  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

    double frame_timer = 0;

    bool step = false;
    bool paused_ = false;

private:
    FrameQueue *picture_queue = nullptr;
    std::thread *render_thread_ = nullptr;
    std::mutex *render_mutex_ = nullptr;


    bool force_refresh_ = false;

    std::shared_ptr<MessageContext> msg_ctx_{};

private:

    void VideoRenderThread();

    double VideoPictureDuration(Frame *vp, Frame *next_vp) const;

    double ComputeTargetDelay(double delay) const;

    void RenderPicture();

public:

    VideoRender();

    ~VideoRender();

    void Init(PacketQueue *video_queue, ClockContext *clock_ctx, std::shared_ptr<MessageContext> msg_ctx);

    bool Start();

    void Stop();

    double DrawFrame();

    int PushFrame(AVFrame *frame, double pts, double duration, int pkt_serial);

    double GetVideoAspectRatio() const;

};


#endif //FFPLAYER_FFP_VIDEO_RENDER_H
