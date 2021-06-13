// ffplayer.cpp: 定义应用程序的入口点。
//

#if __APPLE__
#include "macos_audio_renderer_sink.h"
#endif

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

#define CURSOR_HIDE_DELAY 1000000

static SDL_Window *window;

static float seek_interval = 10;
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

static double volume_before_mute = 1;

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

void OnVideoSizeChanged(std::shared_ptr<MediaPlayer> media_player, int video_width, int video_height) {
  int width = video_width, height = video_height;
  check_screen_size(width, height);
  set_default_window_size(width, height);
  DLOG(INFO) << "on video size changed: width = " << width << "height = " << height;
  int w, h;
  w = screen_width ? screen_width : default_width;
  h = screen_height ? screen_height : default_height;

  if (!window_title)
    window_title = "Lychee";
  SDL_SetWindowTitle(window, window_title);

  DLOG(INFO) << "set_default_window_size width = " << w << ", height = " << h;
  SDL_SetWindowSize(window, w, h);
  SDL_SetWindowPosition(window, screen_left, screen_top);
  if (is_full_screen)
    SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
  SDL_ShowWindow(window);
}

namespace {

class SdlLycheePlayerExample;

class SDLEventHandler {
 public:
  explicit SDLEventHandler(const weak_ptr<SdlLycheePlayerExample> &media_player) : media_player_(media_player) {}

  void HandleSdlEvent(const SDL_Event &event);

 private:
  std::weak_ptr<SdlLycheePlayerExample> media_player_;

  static void HandleKeyEvent(SDL_Keycode keycode, const std::shared_ptr<SdlLycheePlayerExample> &player_app);

  static void toggle_full_screen() {
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  }

};

class SdlLycheePlayerExample : public std::enable_shared_from_this<SdlLycheePlayerExample> {

 public:

  explicit SdlLycheePlayerExample(
      const vector<std::string> &input_files,
      std::shared_ptr<SDL_Renderer> renderer
  );

  void Start();

  const std::shared_ptr<MediaPlayer> &current_player() { return current_player_; }

  void SkipToPrevious();

  void SkipToNext();

 private:

  std::vector<std::string> input_files_;
  int playing_file_index_;
  std::unique_ptr<TaskRunner> task_runner_;

  std::shared_ptr<SDL_Renderer> renderer_;

  std::shared_ptr<MediaPlayer> current_player_;

  std::shared_ptr<SDLEventHandler> event_handler_;

  void SdlEventLoop() {
    DCHECK(task_runner_->BelongsToCurrentThread());
    SDL_Event sdl_event;
    if (event_handler_ && SDL_PollEvent(&sdl_event)) {
      event_handler_->HandleSdlEvent(sdl_event);
    }
    task_runner_->PostDelayedTask(FROM_HERE,
                                  TimeDelta::FromMilliseconds(2),
                                  std::bind(&SdlLycheePlayerExample::SdlEventLoop, this));
  }

  void PlayItem(const std::string &input_file) {
    auto video_renderer_sink = std::make_unique<SdlVideoRendererSink>(*task_runner_, renderer_);
#if __APPLE__
    auto audio_renderer_sink = std::make_unique<MacosAudioRendererSink>();
#else
    auto audio_renderer_sink = std::make_unique<SdlAudioRendererSink>();
#endif
    auto player = std::make_shared<MediaPlayer>(
        std::move(video_renderer_sink),
        std::move(audio_renderer_sink),
        TaskRunner::CreateFromCurrent());
    player->SetVolume(0.5);
    // TODO temp solution.
    task_runner_->PostDelayedTask(FROM_HERE, TimeDelta::FromMilliseconds(30), [player]() {
      OnVideoSizeChanged(player, 1280, 720);
    });

    if (player->OpenDataSource(input_file.c_str()) < 0) {
      LOG(FATAL) << "failed to open file";
      do_exit();
    }
    // perform play when start.
    player->SetPlayWhenReady(true);
    current_player_ = player;
  }

};

SdlLycheePlayerExample::SdlLycheePlayerExample(
    const vector<std::string> &input_files,
    std::shared_ptr<SDL_Renderer> renderer)
    : input_files_(input_files),
      renderer_(std::move(renderer)),
      playing_file_index_(0),
      task_runner_(std::make_unique<TaskRunner>(base::MessageLooper::Current())) {
  DCHECK(!input_files.empty());
  DCHECK(task_runner_);
}

void SdlLycheePlayerExample::Start() {
  DCHECK_GE(playing_file_index_, 0);
  DCHECK_LT(playing_file_index_, input_files_.size());
  PlayItem(input_files_[playing_file_index_]);

  event_handler_ = std::make_shared<SDLEventHandler>(shared_from_this());
  task_runner_->PostTask(FROM_HERE, std::bind(&SdlLycheePlayerExample::SdlEventLoop, this));
}

void SdlLycheePlayerExample::SkipToPrevious() {
  playing_file_index_ = playing_file_index_ - 1;
  if (playing_file_index_ < 0) {
    playing_file_index_ = (int) input_files_.size() - 1;
  }
  PlayItem(input_files_[playing_file_index_]);
}

void SdlLycheePlayerExample::SkipToNext() {
  TRACE_METHOD_DURATION(1000);
  playing_file_index_ = playing_file_index_ + 1;
  if (playing_file_index_ >= input_files_.size()) {
    playing_file_index_ = 0;
  }
  PlayItem(input_files_[playing_file_index_]);
}

// static
void SDLEventHandler::HandleKeyEvent(SDL_Keycode keycode, const std::shared_ptr<SdlLycheePlayerExample> &player_app) {
  /* Step size for volume control*/
  static const double kExampleVolumeStep = 0.1;
  auto player = player_app->current_player();
  DCHECK(player);
  switch (keycode) {
    case SDLK_ESCAPE:
    case SDLK_q: {
      do_exit();
      return;
    }
    case SDLK_f:toggle_full_screen();
      break;
    case SDLK_p:
    case SDLK_SPACE: {
      player->SetPlayWhenReady(!player->IsPlayWhenReady());
      break;
    }
    case SDLK_m: {
      double volume = player->GetVolume();
      if (volume != 0) {
        player->SetVolume(0);
        volume_before_mute = volume;
      } else {
        player->SetVolume(volume_before_mute);
      }
      break;
    }
    case SDLK_KP_MULTIPLY:
    case SDLK_0: {
      player->SetVolume(player->GetVolume() + kExampleVolumeStep);
      DLOG(INFO) << "update volume to: " << player->GetVolume();
      break;
    }
    case SDLK_KP_DIVIDE:
    case SDLK_9: {
      player->SetVolume(player->GetVolume() - kExampleVolumeStep);
      DLOG(INFO) << "update volume to: " << player->GetVolume();
      break;
    }
    case SDLK_s:  // S: Step to next frame
      break;
    case SDLK_a:break;
    case SDLK_v: {
      dump_status = !dump_status;
      break;
    }
    case SDLK_c:break;
    case SDLK_t:break;
    case SDLK_w:break;
//        case SDLK_PAGEUP:
//          if (player->GetChapterCount() <= 1) {
//            incr = 600.0;
//            goto do_seek;
//          }
//          player->SeekToChapter(player->GetChapterCount() + 1);
//          break;
//        case SDLK_PAGEDOWN:
//          if (player->GetChapterCount() <= 1) {
//            incr = -600.0;
//            goto do_seek;
//          }
//          player->SeekToChapter(player->GetChapterCount() - 1);
//          break;
    case SDLK_LEFT: {
      player->Seek(player->GetCurrentPosition() - 10.0);
      break;
    }
    case SDLK_RIGHT: {
      player->Seek(player->GetCurrentPosition() + 10.0);
      break;
    }
    case SDLK_UP: {
      player_app->SkipToPrevious();
      break;
    }
    case SDLK_DOWN: {
      player_app->SkipToNext();
      break;
    }
    default:break;
  }
}

void SDLEventHandler::HandleSdlEvent(const SDL_Event &event) {

  TRACE_METHOD_DURATION(10000);
  _logging_trace_method_duration.stream() << event.type;

  double x;

  auto media = media_player_.lock();
  if (!media) {
    return;
  }
  auto player = media->current_player();

  switch (event.type) {
    case SDL_KEYDOWN: {
      HandleKeyEvent(event.key.keysym.sym, media);
      break;
    }
    case SDL_MOUSEBUTTONDOWN:
      if (exit_on_mousedown) {
        do_exit();
        break;
      }
      if (event.button.button == SDL_BUTTON_LEFT) {
        static int64_t last_mouse_left_click = 0;
        if (av_gettime_relative() - last_mouse_left_click <= 500000) {
          toggle_full_screen();
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
    default:break;
  }
}

} // namespace

int main(int argc, char *argv[]) {
  std::vector<std::string> input_files(argc - 1);
  for (int i = 0; i < argc - 1; ++i) {
    input_files[i] = argv[i + 1];
  }
  CHECK_GT(input_files.size(), 0) << "An input file must be specified";

  MediaPlayer::GlobalInit();
#ifndef __APPLE__
  media::sdl::InitSdlAudio();
#endif

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
    window = SDL_CreateWindow("Lychee", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                              default_height, window_flags);

    DCHECK(window) << "failed to create window." << SDL_GetError();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    renderer = std::shared_ptr<SDL_Renderer>(
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
        [](SDL_Renderer *ptr) {
          SDL_DestroyRenderer(ptr);
        });
    if (!renderer) {
      DLOG(WARNING) << "failed to initialize a hardware accelerated renderer: " << SDL_GetError();
      renderer = std::shared_ptr<SDL_Renderer>(SDL_CreateRenderer(window, -1, 0), [](SDL_Renderer *ptr) {
        SDL_DestroyRenderer(ptr);
      });
    }

    DCHECK(renderer) << "failed to create renderer: " << SDL_GetError();

    if (!SDL_GetRendererInfo(renderer.get(), &renderer_info))
      av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);

    DCHECK_GT(renderer_info.num_texture_formats, 0)
        << "Failed to create window or renderer: " << SDL_GetError();
  }

  auto looper = base::MessageLooper::Create("main_task", 16);
  looper->Prepare();
  auto player = make_shared<SdlLycheePlayerExample>(input_files, renderer);
  looper->PostTask(FROM_HERE, std::bind(&SdlLycheePlayerExample::Start, player));
  looper->Loop();

  return 0;
}

