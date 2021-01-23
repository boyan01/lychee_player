// ffplayer.cpp: 定义应用程序的入口点。
//

#include <signal.h>

#include "ffplayer/ffplayer.h"
#include "ffplayer/utils.h"

static SDL_Window *window;

static float seek_interval = 10;
static int exit_on_keydown;
static int exit_on_mousedown;
static int borderless;
static int alwaysontop;
static int64_t cursor_last_shown;
static int cursor_hidden;

static int is_full_screen;

#ifdef _WIN32
static const char *input_filename = "C:/Users/boyan/Desktop/mojito.mp4";
#elif __LINUX__
static const char *input_filename = "/home/boyan/mojito.mp4";
#endif

static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

static void sigterm_handler(int sig) {
    exit(123);
}

static void do_exit(CPlayer *player) {
    if (player->is) {
        ffplayer_free_player(player);
    }
    if (player->renderer)
        SDL_DestroyRenderer(player->renderer);
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

static void toggle_full_screen(VideoState *is) {
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void update_volume(VideoState *is, int sign, double step) {
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double) SDL_MIX_MAXVOLUME) / log(10))
                                           : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0,
                               SDL_MIX_MAXVOLUME);
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

static void refresh_loop_wait_event(CPlayer *player, SDL_Event *event) {
    VideoState *is = player->is;
    double remaining_time = 0.0;
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
            av_usleep((int64_t) (remaining_time * 1000000.0));
        remaining_time = REFRESH_RATE;
        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh)) {
            remaining_time = ffplayer_draw_frame(player);
        }
        SDL_PumpEvents();
    }
}

static void toggle_audio_display(VideoState *is) {
    int next = is->show_mode;
    do {
        next = (next + 1) % SHOW_MODE_NB;
    } while (next != is->show_mode &&
             (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));
    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = next;
    }
}

/* handle an event sent by the GUI */
_Noreturn static void event_loop(CPlayer *player) {
    VideoState *cur_stream = player->is;
    SDL_Event event;
    double incr;

    for (;;) {
        double x;
        refresh_loop_wait_event(player, &event);
        switch (event.type) {
            case SDL_KEYDOWN:
                if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                    do_exit(player);
                    break;
                }
                // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
                if (!cur_stream->width)
                    continue;
                switch (event.key.keysym.sym) {
                    case SDLK_f:
                        toggle_full_screen(cur_stream);
                        cur_stream->force_refresh = 1;
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
                        update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_KP_DIVIDE:
                    case SDLK_9:
                        update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_s:  // S: Step to next frame
                        step_to_next_frame(player);
                        break;
                    case SDLK_a:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                        break;
                    case SDLK_v:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                        break;
                    case SDLK_c:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_t:
                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_w:
#if CONFIG_AVFILTER
                        if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                            if (++cur_stream->vfilter_idx >= nb_vfilters)
                                cur_stream->vfilter_idx = 0;
                        } else {
                            cur_stream->vfilter_idx = 0;
                            toggle_audio_display(cur_stream);
                        }
#else
                        toggle_audio_display(cur_stream);
#endif
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
                        //     if (pos < 0 && cur_stream->video_stream >= 0)
                        //         pos = frame_queue_last_pos(&cur_stream->pictq);
                        //     if (pos < 0 && cur_stream->audio_stream >= 0)
                        //         pos = frame_queue_last_pos(&cur_stream->sampq);
                        //     if (pos < 0)
                        //         pos = avio_tell(cur_stream->ic->pb);
                        //     if (cur_stream->ic->bit_rate)
                        //         incr *= cur_stream->ic->bit_rate / 8.0;
                        //     else
                        //         incr *= 180000.0;
                        //     pos += incr;
                        //     stream_seek(cur_stream, pos, incr, 1);
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
                        toggle_full_screen(cur_stream);
                        cur_stream->force_refresh = 1;
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

                double dest = (x / cur_stream->width) * ffplayer_get_duration(player);
                ffplayer_seek_to_position(player, dest);

                break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        screen_width = cur_stream->width = event.window.data1;
                        screen_height = cur_stream->height = event.window.data2;
                        if (cur_stream->vis_texture) {
                            SDL_DestroyTexture(cur_stream->vis_texture);
                            cur_stream->vis_texture = NULL;
                        }
                    case SDL_WINDOWEVENT_EXPOSED:
                        cur_stream->force_refresh = 1;
                }
                break;
            case SDL_QUIT:
            case FF_QUIT_EVENT:
                do_exit(player);
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
    CPlayer *player = op;
    AVDictionaryEntry *t;
    if (!window_title && (t = av_dict_get(player->is->ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);
}


static void on_message(void *player, int what, int64_t arg1, int64_t arg2) {
    av_log(NULL, AV_LOG_INFO, "on msg(%d): arg1 = %ld, arg2 = %ld \n", what, arg1, arg2);
    switch (what) {
        case FFP_MSG_VIDEO_FRAME_LOADED:
            set_default_window_size(arg1, arg2);
            int w, h;
            w = screen_width ? screen_width : default_width;
            h = screen_height ? screen_height : default_height;

            if (!window_title)
                window_title = input_filename;
            SDL_SetWindowTitle(window, window_title);

            printf("set_default_window_size : %d , %d \n", w, h);
            SDL_SetWindowSize(window, w, h);
            SDL_SetWindowPosition(window, screen_left, screen_top);
            if (is_full_screen)
                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_ShowWindow(window);
            break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    if (!input_filename) {
        av_log(NULL, AV_LOG_FATAL, "An input file must be specified\n");
        exit(1);
    }

    ffplayer_global_init(NULL);

    signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    CPlayer *player = ffplayer_alloc_player();
    if (!player) {
        printf("failed to alloc player");
        return -1;
    }
    player->show_status = true;
    ffplayer_set_volume(player, 100);

    int flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (player->audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }
    if (player->display_disable)
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

    if (!player->display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
        av_log(NULL, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow("ffplay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                                  default_height, flags);
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
    }
    player->renderer = renderer;
    player->on_load_metadata = on_load_metadata;
    player->on_message = on_message;

    ffplayer_open_file(player, input_filename);
    if (!player->is) {
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(NULL);
    }

    event_loop(player);

    return 0;
}
