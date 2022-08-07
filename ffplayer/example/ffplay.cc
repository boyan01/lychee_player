// ffplayer.cpp: 定义应用程序的入口点。
//

#include <csignal>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

#include "logging.h"
#include "media_player.h"
#include "render_video_sdl.h"
#include "sdl_utils.h"

#include "app_window.h"

#ifdef LYCHEE_OSX
#include "audio_render_core_audio.h"
#define AudioRenderImpl AudioRenderCoreAudio
#elif defined(LYCHEE_ENABLE_SDL)
#define AudioRenderImpl AudioRenderSdl
#include "audio_render_sdl.h"
#endif

extern "C" {
#include "libavutil/avstring.h"
}

using namespace std;

#define FF_DRAW_EVENT (SDL_USEREVENT + 2)
#define FF_MSG_EVENT (SDL_USEREVENT + 3)
/* Step size for volume control*/
#define SDL_VOLUME_STEP (10)

#define CURSOR_HIDE_DELAY 1000000

static SDL_Window* window;

static float seek_interval = 10;
static int exit_on_keydown;
static int exit_on_mousedown;
static int borderless;
static int alwaysontop;
static int64_t cursor_last_shown;
static int cursor_hidden;

static int is_full_screen;

static int volume_before_mute = 1;

static const char* window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;

static bool dump_status = false;

struct MessageData {
  MediaPlayer* player = nullptr;
  int32_t what = 0;
  int64_t arg1 = 0;
  int64_t arg2 = 0;
};

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

static void refresh_loop_wait_event(MediaPlayer* player, SDL_Event* event) {
  double remaining_time = 0.0;
  SDL_PumpEvents();
  while (
      !SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
    if (!cursor_hidden &&
        av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
      SDL_ShowCursor(0);
      cursor_hidden = 1;
    }
    if (remaining_time > 0.0)
      av_usleep((unsigned)(remaining_time * 1000000.0));
    auto* render = dynamic_cast<SdlVideoRender*>(player->GetVideoRender());
    remaining_time = render->DrawFrame();
    if (dump_status) {
      player->DumpStatus();
    }
    SDL_PumpEvents();
  }
}

static void set_default_window_size(int width, int height) {
  SDL_Rect rect;
  int max_width = screen_width ? screen_width : INT_MAX;
  int max_height = screen_height ? screen_height : INT_MAX;
  if (max_width == INT_MAX && max_height == INT_MAX)
    max_height = height;
  AVRational rational = {1, 1};
  media::sdl::calculate_display_rect(&rect, 0, 0, max_width, max_height, width,
                                     height, rational);
  default_width = rect.w;
  default_height = rect.h;
}

/// limit max show size to half of screen.
void check_screen_size(int64_t& width, int64_t& height) {
  SDL_DisplayMode display_mode;

  int window_index = 0;
  if (window) {
    window_index = SDL_GetWindowDisplayIndex(window);
  }
  SDL_GetCurrentDisplayMode(window_index, &display_mode);

  auto scale_w = double(width) / display_mode.w;
  auto scale_h = double(height) / display_mode.h;
  auto scale = min(min(scale_h, scale_w), 0.5);
  width = int64_t(display_mode.w * scale);
  height = int64_t(display_mode.h * scale);
}

static void on_message(MediaPlayer* player,
                       int what,
                       int64_t arg1,
                       int64_t arg2) {
  //    av_log(nullptr, AV_LOG_INFO, "on msg(%d): arg1 = %ld, arg2 = %ld \n",
  //    what, arg1, arg2);
  switch (what) {
    case FFP_MSG_VIDEO_FRAME_LOADED: {
      check_screen_size(arg1, arg2);
      set_default_window_size((int)arg1, (int)arg2);
      std::cout << "FFP_MSG_VIDEO_FRAME_LOADED: width = " << arg1
                << "height = " << arg2 << endl;
      int w, h;
      w = screen_width ? screen_width : default_width;
      h = screen_height ? screen_height : default_height;

      window_title = strdup(player->GetUrl());
      SDL_SetWindowTitle(window, window_title);

      printf("set_default_window_size : %d , %d \n", w, h);
      SDL_SetWindowSize(window, w, h);
      SDL_SetWindowPosition(window, screen_left, screen_top);
      if (is_full_screen)
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
      SDL_ShowWindow(window);
      break;
    }
    case MEDIA_MSG_PLAYER_STATE_CHANGED:
      printf("FFP_MSG_PLAYBACK_STATE_CHANGED : %lld \n", arg1);
      break;
    case FFP_MSG_BUFFERING_TIME_UPDATE:
      printf("FFP_MSG_BUFFERING_TIME_UPDATE: %f.  %f:%f \n",
             double(arg1) / 1000.0, player->GetCurrentPosition(),
             player->GetDuration());
      break;
    case FFP_MSG_AV_METADATA_LOADED: {
      const char* title = player->GetMetadataDict("title");
      if (!window_title && title)
        window_title = av_asprintf("%s - %s", title, player->GetUrl());
      break;
    }
    default:
      break;
  }
}

namespace {

class SdlLycheePlayerExample;

class SDLEventHandler {
 public:
  explicit SDLEventHandler(const weak_ptr<SdlLycheePlayerExample>& media_player)
      : media_player_(media_player) {}

  void HandleSdlEvent(const SDL_Event& event);

 private:
  std::weak_ptr<SdlLycheePlayerExample> media_player_;

  static void HandleKeyEvent(
      SDL_Keycode keycode,
      const std::shared_ptr<SdlLycheePlayerExample>& player_app);

  static void toggle_full_screen() {
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window,
                            is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  }
};

class SdlLycheePlayerExample
    : public std::enable_shared_from_this<SdlLycheePlayerExample> {
 public:
  explicit SdlLycheePlayerExample(const vector<std::string>& input_files,
                                  std::shared_ptr<SDL_Renderer> renderer);

  void Start();

  const std::shared_ptr<MediaPlayer>& current_player() {
    return current_player_;
  }

  void SkipToPrevious();

  void SkipToNext();

 private:
  std::vector<std::string> input_files_;
  int playing_file_index_;

  std::shared_ptr<SDL_Renderer> renderer_;

  std::shared_ptr<MediaPlayer> current_player_;

  std::shared_ptr<SDLEventHandler> event_handler_;

  void SdlEventLoop() {
    SDL_Event sdl_event;
    while (true) {
      refresh_loop_wait_event(current_player_.get(), &sdl_event);
      if (event_handler_) {
        event_handler_->HandleSdlEvent(sdl_event);
      }
    }
  }

  void PlayItem(const std::string& input_file) {
    current_player_ = nullptr;
    SDL_FlushEvent(FF_MSG_EVENT);

    auto video_render = std::make_unique<SdlVideoRender>(renderer_);
    auto audio_render = std::make_unique<AudioRenderImpl>();
    auto player = std::make_shared<MediaPlayer>(std::move(video_render),
                                                std::move(audio_render));
    player->SetVolume(50);
    player->SetMessageHandleCallback(
        [player_ptr(player.get())](int what, int64_t arg1, int64_t arg2) {
          SDL_Event event;
          event.type = FF_MSG_EVENT;
          auto* data = new MessageData;
          event.user.data1 = data;
          data->player = player_ptr;
          data->what = what;
          data->arg1 = arg1;
          data->arg2 = arg2;
          SDL_PushEvent(&event);
        });
    if (player->OpenDataSource(input_file.c_str()) < 0) {
      LOG(FATAL) << "failed to open file";
      do_exit();
    }
    // perform play when started.
    player->SetPlayWhenReady(true);

    current_player_ = player;
  }
};

SdlLycheePlayerExample::SdlLycheePlayerExample(
    const vector<std::string>& input_files,
    std::shared_ptr<SDL_Renderer> renderer)
    : input_files_(input_files),
      renderer_(std::move(renderer)),
      playing_file_index_(0) {
  DCHECK(!input_files.empty());
}

void SdlLycheePlayerExample::Start() {
  DCHECK_GE(playing_file_index_, 0);
  DCHECK_LT(playing_file_index_, input_files_.size());
  PlayItem(input_files_[playing_file_index_]);
  event_handler_ = std::make_shared<SDLEventHandler>(shared_from_this());
  SdlLycheePlayerExample::SdlEventLoop();
}

void SdlLycheePlayerExample::SkipToPrevious() {
  playing_file_index_ = playing_file_index_ - 1;
  if (playing_file_index_ < 0) {
    playing_file_index_ = (int)input_files_.size() - 1;
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
void SDLEventHandler::HandleKeyEvent(
    SDL_Keycode keycode,
    const std::shared_ptr<SdlLycheePlayerExample>& player_app) {
  /* Step size for volume control*/
  static const int kExampleVolumeStep = 10;
  auto player = player_app->current_player();
  DCHECK(player);
  switch (keycode) {
    case SDLK_ESCAPE:
    case SDLK_q: {
      do_exit();
      return;
    }
    case SDLK_f:
      toggle_full_screen();
      break;
    case SDLK_p:
    case SDLK_SPACE: {
      player->SetPlayWhenReady(!player->IsPlayWhenReady());
      break;
    }
    case SDLK_m: {
      int volume = player->GetVolume();
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
    case SDLK_a:
      break;
    case SDLK_v: {
      dump_status = !dump_status;
      break;
    }
    case SDLK_c:
      break;
    case SDLK_t:
      break;
    case SDLK_w:
      break;
    case SDLK_LEFT: {
      player->Seek(std::max(player->GetCurrentPosition() - 10, 0.0));
      break;
    }
    case SDLK_RIGHT: {
      player->Seek(player->GetCurrentPosition() + 10);
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
    default:
      break;
  }
}

void SDLEventHandler::HandleSdlEvent(const SDL_Event& event) {
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

          auto* render =
              dynamic_cast<SdlVideoRender*>(player->GetVideoRender());
          render->screen_height = screen_height;
          render->screen_width = screen_width;
          render->DestroyTexture();
        }
        case SDL_WINDOWEVENT_EXPOSED:
          //                        is->force_refresh = 1;
          break;
      }
      break;
    case SDL_QUIT:
      do_exit();
      return;
    case FF_MSG_EVENT: {
      auto* msg = static_cast<MessageData*>(event.user.data1);
      on_message(msg->player, msg->what, msg->arg1, msg->arg2);
      delete msg;
      break;
    }
    default:
      break;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<std::string> input_files(argc - 1);
  for (int i = 0; i < argc - 1; ++i) {
    input_files[i] = argv[i + 1];
  }
  CHECK_GT(input_files.size(), 0) << "An input file must be specified";

  signal(SIGINT, sigterm_handler);  /* Interrupt (ANSI).    */
  signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

  MediaPlayer::GlobalInit();

  AppWindow::Initialize();

  window = AppWindow::Instance()->GetWindow();

  auto player = make_shared<SdlLycheePlayerExample>(
      input_files, AppWindow::Instance()->GetRenderer());
  player->Start();

  return 0;
}
