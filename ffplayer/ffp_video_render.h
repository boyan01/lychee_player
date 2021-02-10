//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_VIDEO_RENDER_H
#define FFPLAYER_FFP_VIDEO_RENDER_H


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
    void (*on_render)(FFP_VideoRenderContext *video_render_ctx, Frame *frame) = nullptr;

    void (*on_texture_updated)(FFP_VideoRenderContext *video_render_ctx) = nullptr;

};

struct FFP_VideoRenderContext {
    bool abort_render;
    bool render_attached;

    std::thread *render_thread_;
    std::mutex *render_mutex_;

    bool first_video_frame_loaded = false;
    int frame_width = 0;
    int frame_height = 0;
    FFP_VideoRenderCallback *render_callback_;
};


#endif //FFPLAYER_FFP_VIDEO_RENDER_H
