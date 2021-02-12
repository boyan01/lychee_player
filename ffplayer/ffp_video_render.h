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

struct FFP_VideoRenderContext;

struct FFP_VideoRenderCallback {
    void *opacity = nullptr;

    /**
     *
     * called by video render thread.
     *
     * @param video_render_ctx
     * @param frame
     */
    std::function<void(FFP_VideoRenderContext *, Frame *)> on_render = nullptr;

    void (*on_texture_updated)(FFP_VideoRenderContext *video_render_ctx) = nullptr;

};

struct FFP_VideoRenderContext {
    bool abort_render = false;
    bool render_attached = false;
    bool first_video_frame_loaded = false;
    bool first_video_frame_rendered = false;
    int frame_width = 0;
    int frame_height = 0;
    FFP_VideoRenderCallback *render_callback_ = nullptr;

private:
    std::thread *render_thread_;
    std::mutex *render_mutex_;
public:
    bool Start(CPlayer *player);

    void Stop(CPlayer *player);

    double DrawFrame(CPlayer *player);

};


#endif //FFPLAYER_FFP_VIDEO_RENDER_H
