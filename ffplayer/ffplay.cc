// ffplayer.cpp: 定义应用程序的入口点。
//

#include <csignal>
#include <iostream>

extern "C" {
#include "ffplayer/ffplayer.h"
#include "ffplayer/utils.h"
}

using namespace std;

#define FF_DRAW_EVENT    (SDL_USEREVENT + 2)
/* Step size for volume control*/
#define SDL_VOLUME_STEP (10)

static SDL_Window *window;

static float seek_interval = 10;
static int exit_on_keydown;
static int exit_on_mousedown;
static int borderless;
static int alwaysontop;
static int64_t cursor_last_shown;
static int cursor_hidden;

static int is_full_screen;

static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

typedef struct DrawEvent_ {
    FFP_VideoRenderContext_ *context;
    Frame *frame;
    int64_t render_time;

    void (*draw)(FFP_VideoRenderContext *c, Frame *f);
} DrawEvent;

static void sigterm_handler(int sig) {
    exit(123);
}

static void do_exit(CPlayer *player) {
    if (player->is) {
        ffplayer_free_player(player);
    }
    if (player->video_render_ctx->renderer)
        SDL_DestroyRenderer(player->video_render_ctx->renderer);
    if (window)
        SDL_DestroyWindow(window);
    // uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (player->show_status)
        printf("\n");
    SDL_Quit();
    av_log(NULL, AV_LOG_QUIET, "%s", "");
    exit(0);
}

static void toggle_full_screen() {
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void step_to_next_frame(CPlayer *player) {
    /* if the stream is paused unpause it, then step */
    if (ffplayer_is_paused(player))
        ffplayer_toggle_pause(player);
    player->is->step = 1;
}

static void stream_cycle_channel(VideoState *is, int codec_type) {
#if 0
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = NULL;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, NULL, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    if (st->codecpar->sample_rate != 0 &&
                        st->codecpar->channels != 0)
                        goto the_end;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                case AVMEDIA_TYPE_SUBTITLE:
                    goto the_end;
                default:
                    break;
            }
        }
    }
the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(codec_type),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);

#endif
}

static void refresh_loop_wait_event(SDL_Event *event) {
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        SDL_PumpEvents();
    }
}


/* handle an event sent by the GUI */
static void event_loop(CPlayer *player) {
    VideoState *is = player->is;
    SDL_Event event;
    double incr;

#pragma ide diagnostic ignored "EndlessLoop"
    for (;;) {
        double x;
        refresh_loop_wait_event(&event);
        switch (event.type) {
            case SDL_KEYDOWN:
                if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    do_exit(player);
                    break;
                }
                // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
                if (!player->video_render_ctx->width)
                    continue;
                switch (event.key.keysym.sym) {
                    case SDLK_f:
                        toggle_full_screen();
                        is->force_refresh = 1;
                        break;
                    case SDLK_p:
                    case SDLK_SPACE:
                        ffplayer_toggle_pause(player);
                        break;
                    case SDLK_m:
                        ffplayer_set_mute(player, !ffplayer_is_mute(player));
                        break;
                    case SDLK_KP_MULTIPLY:
                    case SDLK_0:
                        ffp_set_volume(player, ffp_get_volume(player) + SDL_VOLUME_STEP);
                        break;
                    case SDLK_KP_DIVIDE:
                    case SDLK_9:
                        ffp_set_volume(player, ffp_get_volume(player) - SDL_VOLUME_STEP);
                        break;
                    case SDLK_s:  // S: Step to next frame
                        step_to_next_frame(player);
                        break;
                    case SDLK_a:
                        stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
                        break;
                    case SDLK_v:
                        stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
                        break;
                    case SDLK_c:
                        stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
                        stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
                        stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_t:
                        stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_w:
                        break;
                    case SDLK_PAGEUP:
                        if (ffplayer_get_chapter_count(player) <= 1) {
                            incr = 600.0;
                            goto do_seek;
                        }
                        ffplayer_seek_to_chapter(player, ffplayer_get_current_chapter(player) + 1);
                        break;
                    case SDLK_PAGEDOWN:
                        if (ffplayer_get_chapter_count(player) <= 1) {
                            incr = -600.0;
                            goto do_seek;
                        }
                        ffplayer_seek_to_chapter(player, ffplayer_get_current_chapter(player) - 1);
                        break;
                    case SDLK_LEFT:
                        incr = seek_interval ? -seek_interval : -10.0;
                        goto do_seek;
                    case SDLK_RIGHT:
                        incr = seek_interval ? seek_interval : 10.0;
                        goto do_seek;
                    case SDLK_UP:
                        incr = 60.0;
                        goto do_seek;
                    case SDLK_DOWN:
                        incr = -60.0;
                    do_seek:
                        // if (player->seek_by_bytes) {
                        //     pos = -1;
                        //     if (pos < 0 && is->video_stream >= 0)
                        //         pos = frame_queue_last_pos(&is->pictq);
                        //     if (pos < 0 && is->audio_stream >= 0)
                        //         pos = frame_queue_last_pos(&is->sampq);
                        //     if (pos < 0)
                        //         pos = avio_tell(is->ic->pb);
                        //     if (is->ic->bit_rate)
                        //         incr *= is->ic->bit_rate / 8.0;
                        //     else
                        //         incr *= 180000.0;
                        //     pos += incr;
                        //     stream_seek(is, pos, incr, 1);
                        // }
                        printf("ffplayer_seek_to_position from: %0.2f , to: %0.2f .\n",
                               ffplayer_get_current_position(player), ffplayer_get_current_position(player) + incr);
                        ffplayer_seek_to_position(player, ffplayer_get_current_position(player) + incr);
                        break;
                    default:
                        break;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (exit_on_mousedown) {
                    do_exit(player);
                    break;
                }
                if (event.button.button == SDL_BUTTON_LEFT) {
                    static int64_t last_mouse_left_click = 0;
                    if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                        toggle_full_screen();
                        is->force_refresh = 1;
                        last_mouse_left_click = 0;
                    } else {
                        last_mouse_left_click = av_gettime_relative();
                    }
                }
            case SDL_MOUSEMOTION:
                if (cursor_hidden) {
                    SDL_ShowCursor(1);
                    cursor_hidden = 0;
                }
                cursor_last_shown = av_gettime_relative();
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button != SDL_BUTTON_RIGHT)
                        break;
                    x = event.button.x;
                } else {
                    if (!(event.motion.state & SDL_BUTTON_RMASK))
                        break;
                    x = event.motion.x;
                }
                {
                    double dest = (x / player->video_render_ctx->width) * ffplayer_get_duration(player);
                    ffplayer_seek_to_position(player, dest);
                }
                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        screen_width = player->video_render_ctx->width = event.window.data1;
                        screen_height = player->video_render_ctx->height = event.window.data2;
                        ffp_refresh_texture(player);
                    case SDL_WINDOWEVENT_EXPOSED:
                        is->force_refresh = 1;
                }
                break;
            case SDL_QUIT:
                do_exit(player);
                break;
            case FF_DRAW_EVENT: {
                auto *draw_event = static_cast<DrawEvent *>(event.user.data1);
                draw_event->draw(draw_event->context, draw_event->frame);
                cout << "draw delay time: "
                     << (av_gettime_relative() - draw_event->render_time) / 1000 << " milliseconds"
                     << endl;
                delete draw_event;
            }
                break;
            default:
                break;
        }
    }
}

static void set_default_window_size(int width, int height) {
    SDL_Rect rect;
    int max_width = screen_width ? screen_width : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX)
        max_height = height;
    AVRational rational = {1, 1};
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, rational);
    default_width = rect.w;
    default_height = rect.h;
}

static void on_load_metadata(void *op) {
    auto *player = static_cast<CPlayer *>(op);
    AVDictionaryEntry *t;
    if (!window_title && (t = av_dict_get(player->is->ic->metadata, "title", nullptr, 0)))
        window_title = av_asprintf("%s - %s", t->value, player->is->filename);
}


static void on_message(CPlayer *player, int what, int64_t arg1, int64_t arg2) {
//    av_log(NULL, AV_LOG_INFO, "on msg(%d): arg1 = %ld, arg2 = %ld \n", what, arg1, arg2);
    switch (what) {
        case FFP_MSG_VIDEO_FRAME_LOADED:
            set_default_window_size(arg1, arg2);
            int w, h;
            w = screen_width ? screen_width : default_width;
            h = screen_height ? screen_height : default_height;

            if (!window_title)
                window_title = player->is->filename;
            SDL_SetWindowTitle(window, window_title);

            printf("set_default_window_size : %d , %d \n", w, h);
            SDL_SetWindowSize(window, w, h);
            SDL_SetWindowPosition(window, screen_left, screen_top);
            if (is_full_screen)
                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_ShowWindow(window);
            break;
        case FFP_MSG_PLAYBACK_STATE_CHANGED:
            printf("FFP_MSG_PLAYBACK_STATE_CHANGED : %lld \n", arg1);
            break;
        case FFP_MSG_BUFFERING_TIME_UPDATE:
            printf("FFP_MSG_BUFFERING_TIME_UPDATE: %f.  %f:%f \n", arg1 / 1000.0, ffplayer_get_current_position(player),
                   ffplayer_get_duration(player));
            break;
        default:
            break;
    }
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame) {
#if SDL_VERSION_ATLEAST(2, 0, 8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 ||
                  frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
                 frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}


static void render_frame(FFP_VideoRenderContext *context, Frame *frame) {
    static auto callbalc = [](FFP_VideoRenderContext *c, Frame *vp) {
        SDL_RenderPresent(c->renderer);
    };
    auto *draw_event = new DrawEvent();
    draw_event->draw = callbalc;
    draw_event->frame = frame;
    draw_event->context = context;
    draw_event->render_time = av_gettime_relative();
    SDL_Event event;
    event.type = FF_DRAW_EVENT;
    event.user.data1 = static_cast<void *>(draw_event);
    SDL_PushEvent(&event);
}

int main(int argc, char *argv[]) {
    char *input_file = argv[1];
    if (!input_file) {
#ifdef _WIN32
        static const char *input_filename = "C:/Users/boyan/Desktop/mojito.mp4";
#elif __LINUX__
        static const char *input_filename = "/home/boyan/mojito.mp4";
#endif
        input_file = (char *) input_filename;
    }
    if (!input_file) {
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        exit(1);
    }

    ffplayer_global_init(NULL);

    signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    FFPlayerConfiguration config = {
            0,
            0,
            1,
            -1,
            1,
            0,
            1,
    };

    CPlayer *player = ffp_create_player(&config);
    if (!player) {
        printf("failed to alloc player");
        return -1;
    }
    player->show_status = true;
    ffp_set_volume(player, 100);

    uint32_t flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (player->audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }
    if (config.video_disable)
        flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init(flags)) {
        av_log(NULL, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(NULL, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }
    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    SDL_Renderer *renderer;
    SDL_RendererInfo renderer_info = {0};

    if (!config.video_disable) {
        uint32_t window_flags = SDL_WINDOW_HIDDEN;
        if (alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
            window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
        av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (borderless)
            window_flags |= SDL_WINDOW_BORDERLESS;
        else
            window_flags |= SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow("ffplay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                                  default_height, window_flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer) {
                av_log(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
                       SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0);
            }
            if (renderer) {
                if (!SDL_GetRendererInfo(renderer, &renderer_info))
                    av_log(NULL, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
        }
        if (!window || !renderer || !renderer_info.num_texture_formats) {
            av_log(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            do_exit(NULL);
        }
        ffp_set_video_render(player, renderer);
        player->video_render_ctx->render_callback = render_frame;
    }
    player->on_load_metadata = on_load_metadata;
    player->on_message = on_message;

    ffplayer_open_file(player, input_file);
    if (!player->is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    if (ffplayer_is_paused(player)) {
        // perform play when start.
        ffplayer_toggle_pause(player);
    }
    event_loop(player);

    return 0;
}
