// ffplayer.cpp: 定义应用程序的入口点。
//

#include <csignal>
#include <iostream>
#include <cstdint>
#include <utility>

#include "base/logging.h"
#include <base/bind_to_current_loop.h>

#include "ffp_utils.h"
#include "media_player.h"
#include "sdl_utils.h"
#include "sdl_audio_renderer_sink.h"
#include "sdl_video_renderer_sink.h"

extern "C" {
#include "libavutil/avstring.h"
}

using namespace std;
using namespace media;

#define FF_DRAW_EVENT    (SDL_USEREVENT + 2)
#define FF_MSG_EVENT    (SDL_USEREVENT + 3)
const int SDL_UPDATE_FRAME_SIZE = SDL_USEREVENT + 4;
/* Step size for volume control*/
#define SDL_VOLUME_STEP (10)

#define CURSOR_HIDE_DELAY 1000000

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

static bool dump_status = false;

struct MessageData {
  MediaPlayer *player = nullptr;
  int32_t what = 0;
  int64_t arg1 = 0;
  int64_t arg2 = 0;

};

static void on_message(MediaPlayer *player, int what, int64_t arg1, int64_t arg2);

static void sigterm_handler(int sig) {
  exit(123);
}

static void do_exit() {
  if (window)
    SDL_DestroyWindow(window);

  SDL_Quit();
  av_log(nullptr, AV_LOG_QUIET, "%s", "");
  exit(0);
}

static void toggle_full_screen() {
  is_full_screen = !is_full_screen;
  SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void step_to_next_frame(std::shared_ptr<MediaPlayer> player) {
  /* if the stream is play_when_ready_ unpause it, then step */
//  if (player->IsPaused()) {
//    player->TogglePause();
//  }
//    player->is->step = 1;
}

static double refresh_loop_wait_event(std::shared_ptr<MediaPlayer> player, SDL_Event *event) {
  double remaining_time = 0.01;
  SDL_PumpEvents();
  if (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden = 1;
    }
    if (remaining_time > 0.0)
      av_usleep((int64_t) (remaining_time * 1000000.0));
    if (dump_status) {
      player->DumpStatus();
    }
    SDL_PumpEvents();
  }
  return remaining_time;
}

/* handle an event sent by the GUI */
static void event_loop(std::shared_ptr<MediaPlayer> player) {
  SDL_Event event;
  double incr;

  double x;
  double next_frame_delayed = refresh_loop_wait_event(player, &event);
  switch (event.type) {
    case SDL_KEYDOWN:
      if (exit_on_keydown || event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
        do_exit();
        break;
      }
      // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
      if (false) // TODO check if player render start
        break;
      switch (event.key.keysym.sym) {
        case SDLK_f:toggle_full_screen();
//                        is->force_refresh = 1;
          break;
        case SDLK_p:
        case SDLK_SPACE: {
          player->SetPlayWhenReady(!player->IsPlayWhenReady());
          break;
        }
        case SDLK_m:player->SetMute(!player->IsMuted());
          break;
        case SDLK_KP_MULTIPLY:
        case SDLK_0:player->SetVolume(player->GetVolume() + SDL_VOLUME_STEP);
          break;
        case SDLK_KP_DIVIDE:
        case SDLK_9:player->SetVolume(player->GetVolume() - SDL_VOLUME_STEP);
          break;
        case SDLK_s:  // S: Step to next frame
          step_to_next_frame(player);
          break;
        case SDLK_a:break;
        case SDLK_v: {
          dump_status = !dump_status;
          break;
        }
        case SDLK_c:break;
        case SDLK_t:break;
        case SDLK_w:break;
        case SDLK_PAGEUP:
          if (player->GetChapterCount() <= 1) {
            incr = 600.0;
            goto do_seek;
          }
          player->SeekToChapter(player->GetChapterCount() + 1);
          break;
        case SDLK_PAGEDOWN:
          if (player->GetChapterCount() <= 1) {
            incr = -600.0;
            goto do_seek;
          }
          player->SeekToChapter(player->GetChapterCount() - 1);
          break;
        case SDLK_LEFT:incr = -seek_interval;
          goto do_seek;
        case SDLK_RIGHT:incr = seek_interval;
          goto do_seek;
        case SDLK_UP:incr = 60.0;
          goto do_seek;
        case SDLK_DOWN:incr = -60.0;
        do_seek:
          printf("ffplayer_seek_to_position from: %0.2f , to: %0.2f .\n",
                 player->GetCurrentPosition(), player->GetCurrentPosition() + incr);
          player->Seek(player->GetCurrentPosition() + incr);
          break;
        default:break;
      }
      break;
    case SDL_MOUSEBUTTONDOWN:
      if (exit_on_mousedown) {
        do_exit();
        break;
      }
      if (event.button.button == SDL_BUTTON_LEFT) {
        static int64_t last_mouse_left_click = 0;
        if (av_gettime_relative() - last_mouse_left_click <= 500000) {
          toggle_full_screen();
//                        is->force_refresh = 1;
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
        double dest = (x / screen_width) * player->GetDuration();
        player->Seek(dest);
      }
      break;
    case SDL_WINDOWEVENT:
      switch (event.window.event) {
        case SDL_WINDOWEVENT_SIZE_CHANGED: {
          screen_width = event.window.data1;
          screen_height = event.window.data2;
//          auto *render = dynamic_cast<SdlVideoRender *>(player->GetVideoRender());
//          render->screen_width = screen_width;
//          render->screen_height = screen_height;
//          render->DestroyTexture();

          SdlVideoRendererSink::screen_height = screen_height;
          SdlVideoRendererSink::screen_width = screen_width;
        }
        case SDL_WINDOWEVENT_EXPOSED:
//                        is->force_refresh = 1;
          break;
      }
      break;
    case SDL_QUIT:do_exit();
      return;
    case FF_DRAW_EVENT: {
//                auto *video_render_ctx = static_cast<VideoRenderData *>(event.user.data1);
//                SDL_RenderPresent(video_render_ctx->renderer);
//                cout << "draw delay time: "
//                     << (SDL_GetTicks() - event.user.timestamp) << " milliseconds"
//                     << endl;
    }
      break;
    case FF_MSG_EVENT: {
      auto *msg = static_cast<MessageData *>(event.user.data1);
      on_message(msg->player, msg->what, msg->arg1, msg->arg2);
      delete msg;
    }
      break;
    default:break;
  }
  auto *main_task_runner = TaskRunner::current();
  DCHECK(main_task_runner);
  main_task_runner->PostDelayedTask(FROM_HERE,
                                    TimeDelta(int64(next_frame_delayed * 1000000)),
                                    [player] { return event_loop(player); });
}

static void set_default_window_size(int width, int height) {
  SDL_Rect rect;
  int max_width = screen_width ? screen_width : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX)
    max_height = height;
  AVRational rational = {1, 1};
  media::sdl::calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, rational);
  default_width = rect.w;
  default_height = rect.h;
}

/// limit max show size to half of screen.
static void check_screen_size(int &width, int &height) {
  SDL_DisplayMode display_mode;

  int window_index = 0;
  if (window) {
    window_index = SDL_GetWindowDisplayIndex(window);
  }
  SDL_GetCurrentDisplayMode(window_index, &display_mode);

  auto scale_w = double(width) / display_mode.w;
  auto scale_h = double(height) / display_mode.h;
  auto scale = min(min(scale_h, scale_w), 0.5);
  width = display_mode.w * scale;
  height = display_mode.h * scale;
}

static void on_message(MediaPlayer *player, int what, int64_t arg1, int64_t arg2) {
//    av_log(nullptr, AV_LOG_INFO, "on msg(%d): arg1 = %ld, arg2 = %ld \n", what, arg1, arg2);
}

void OnVideoSizeChanged(std::shared_ptr<MediaPlayer> media_player, int video_width, int video_height) {
  int width = video_width, height = video_height;
  check_screen_size(width, height);
  set_default_window_size(width, height);
  DLOG(INFO) << "on video size changed: width = " << width << "height = " << height;
  int w, h;
  w = screen_width ? screen_width : default_width;
  h = screen_height ? screen_height : default_height;

  if (!window_title)
    window_title = media_player->GetUrl();
  SDL_SetWindowTitle(window, window_title);

  DLOG(INFO) << "set_default_window_size width = " << w << ", height = " << h;
  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, screen_left, screen_top);
  if (is_full_screen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window);
}

int main(int argc, char *argv[]) {
  char *input_file = argv[1];
  if (!input_file) {
    LOG(FATAL) << "An input file must be specified";
    exit(1);
  }

  MediaPlayer::GlobalInit();
  media::sdl::InitSdlAudio();

  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  PlayerConfiguration config;

  uint32_t flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
  if (config.audio_disable)
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
    av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
    av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
    exit(1);
  }
  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  std::shared_ptr<SDL_Renderer> renderer;

  if (!config.video_disable) {
    SDL_RendererInfo renderer_info = {nullptr};
    uint32_t window_flags = SDL_WINDOW_HIDDEN;
    if (alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
      window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
    av_log(nullptr, AV_LOG_WARNING, "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
    if (borderless)
      window_flags |= SDL_WINDOW_BORDERLESS;
    else
      window_flags |= SDL_WINDOW_RESIZABLE;
    window = SDL_CreateWindow("ffplay", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, window_flags);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (window) {
      renderer = std::shared_ptr<SDL_Renderer>(
          SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
          [](SDL_Renderer *ptr) {
            SDL_DestroyRenderer(ptr);
          });
      if (!renderer) {
        av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
               SDL_GetError());
        renderer = std::shared_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, 0), [](SDL_Renderer *ptr) {
          SDL_DestroyRenderer(ptr);
        });
      }
      if (renderer) {
        if (!SDL_GetRendererInfo(renderer.get(), &renderer_info))
          av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
      }
    }
    if (!window || !renderer || !renderer_info.num_texture_formats) {
      av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
      do_exit();
    }
  }

  auto *task_runner = new TaskRunner("main_task");
  task_runner->Prepare();

  auto video_renderer_sink = std::make_unique<SdlVideoRendererSink>(task_runner, std::move(renderer));
  auto audio_renderer_sink = std::make_unique<SdlAudioRendererSink>();
  auto player = std::make_shared<MediaPlayer>(std::move(video_renderer_sink), std::move(audio_renderer_sink));
  player->start_configuration = config;
  player->SetVolume(50);

  player->set_on_video_size_changed_callback([player, task_runner](int width, int height) {
    BindToLoop(task_runner, [player, width, height]() {
      OnVideoSizeChanged(player, width, height);
    })();
  });

  // TODO temp solution.
  task_runner->PostDelayedTask(FROM_HERE, TimeDelta(3000), [player]() {
    OnVideoSizeChanged(player, 1280, 720);
  });

  if (player->OpenDataSource(input_file) < 0) {
    LOG(FATAL) << "failed to open file";
    do_exit();
  }

  // perform play when start.
  player->SetPlayWhenReady(true);

  task_runner->PostTask(FROM_HERE, [player]() {
    event_loop(player);
  });
  task_runner->Loop();

  return 0;
}

