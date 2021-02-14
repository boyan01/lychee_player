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
#include "ffp_define.h"
#include "render_base.h"

class VideoRender : public BaseRender {
public:
    bool abort_render = false;
    bool render_attached = false;
    bool first_video_frame_loaded = false;
    bool first_video_frame_rendered = false;
    int frame_width = 0;
    int frame_height = 0;

    /**
    *
    * called by video render thread.
    *
    * @param frame : video frame which should be display.
    */
    std::function<void(Frame *frame)> render_callback;

    int framedrop = -1;

    int frame_drop_count = 0;
    int frame_drop_count_pre = 0;

    std::shared_ptr<ClockContext> clock_context;

    double max_frame_duration = 3600;  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

    double frame_timer = 0;

    bool step = false;
    bool paused_ = false;

private:
    std::unique_ptr<FrameQueue> picture_queue;
    std::thread *render_thread_ = nullptr;
    std::mutex *render_mutex_ = nullptr;


    bool force_refresh_ = false;

    std::shared_ptr<MessageContext> msg_ctx_;

private:

    void VideoRenderThread();

    double VideoPictureDuration(Frame *vp, Frame *next_vp) const;

    double ComputeTargetDelay(double delay) const;

    void RenderPicture();

public:

    VideoRender(const std::shared_ptr<PacketQueue> &video_queue, std::shared_ptr<ClockContext> clock_ctx,
                std::shared_ptr<MessageContext> msg_ctx);

    ~VideoRender();

    bool Start();

    void Abort() override;

    double DrawFrame();

    int PushFrame(AVFrame *frame, double pts, double duration, int pkt_serial);

    double GetVideoAspectRatio() const;

};


#endif //FFPLAYER_FFP_VIDEO_RENDER_H
