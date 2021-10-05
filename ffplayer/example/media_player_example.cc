#include <utility>
#include <external_video_renderer_sink.h>
#ifdef _MEDIA_USE_SDL
#include <sdl_audio_renderer_sink.h>
#define AudioRendererSinkImpl SdlAudioRendererSink
#else
#include <macos_audio_renderer_sink.h>
#define AudioRendererSinkImpl MacosAudioRendererSink
#endif

#include "os/os.h"

#include "media_player.h"
#include "app_window.h"
#include "skia_media_texture.h"

namespace {

class MediaPlayerWindow {
 public:

  static MediaPlayerWindow *instance;

  static void Initialize(std::vector<std::string> input_files) {
    DCHECK(!instance) << "already initialized.";
    instance = new MediaPlayerWindow(std::move(input_files));
  }

  void Refresh() {
    if (!task_runner_.BelongsToCurrentThread()) {
      task_runner_.PostTask(FROM_HERE, std::bind(&MediaPlayerWindow::Refresh, this));
      return;
    }
    DrawWindow();
  }

  static void Close() {
    exit(0);
  }

  void HandleKeyEvent(os::KeyScancode scancode) {
    switch (scancode) {
      case os::kKeyEsc:MediaPlayerWindow::Close();
        break;
      case os::kKey1:
      case os::kKey2:
      case os::kKey3:
      case os::kKey4:
      case os::kKey5:
      case os::kKey6:
      case os::kKey7:
      case os::kKey8:break;
      case os::kKey9: {
        player_->SetVolume(player_->GetVolume() - 0.1);
        break;
      }
      case os::kKey0: {
        player_->SetVolume(player_->GetVolume() + 0.1);
        break;
      }
      case os::kKeySpace: {
        if (player_) {
          player_->SetPlayWhenReady(!player_->IsPlayWhenReady());
        }
        break;
      }
      case os::kKeyDown: {
        PlayItem(playing_index_ + 1);
        break;
      }
      case os::kKeyUp: {
        PlayItem(playing_index_ - 1);
        break;
      }
      case os::kKeyLeft: {
        if (player_) {
          player_->Seek(player_->GetCurrentPosition() - media::TimeDelta::FromSeconds(5));
        }
        break;
      }
      case os::kKeyRight: {
        if (player_) {
          player_->Seek(player_->GetCurrentPosition() + media::TimeDelta::FromSeconds(5));
        }
        break;
      }
      case os::kKeyF:
      case os::kKeyF11: {
        window_->setFullscreen(!window_->isFullscreen());
        DrawWindow();
        break;
      }
      default:
        // Do nothing
        break;
    }
  }

 private:
  os::WindowRef window_;
  media::TaskRunner task_runner_;

  std::shared_ptr<media::MediaPlayer> player_;
  std::vector<std::string> input_files_;

  int playing_index_;

  explicit MediaPlayerWindow(std::vector<std::string> input_files)
      : task_runner_(media::TaskRunner::CreateFromCurrent()),
        player_(),
        input_files_(std::move(input_files)),
        playing_index_(0) {
    window_ = os::instance()->makeWindow(720, 640);
    window_->setTitle("Lychee Player Example");
    window_->setCursor(os::NativeCursor::Arrow);

    auto screen = window_->screen()->workarea();
    auto window_area = window_->frame();

    window_->setFrame(window_area.offset((screen.w - window_area.w) / 2, (screen.h - window_area.h) / 2));
    os::instance()->setGpuAcceleration(true);
    os::instance()->handleWindowResize = [this](os::Window *window) {
      DCHECK(task_runner_.BelongsToCurrentThread());
      DrawWindow();
    };
    task_runner_.PostTask(FROM_HERE, std::bind(&MediaPlayerWindow::DrawWindow, this));
    PlayItem(0);
  }

  void DrawWindow() {
    // Invalidates the whole window to show it on the screen.
    if (window_->isVisible())
      window_->invalidate();
    else
      window_->setVisible(true);

    if (player_) {
      auto surface = window_->surface();

      auto texture_id = dynamic_cast<media::ExternalVideoRendererSink *>(player_->GetVideoRenderSink())->texture_id();
      auto external_surface = SkiaMediaTexture::GetExternalSurface(texture_id);

      if (!external_surface || !external_surface->isValid()) {
        return;
      }

      surface->lock();
      surface->drawSurface(external_surface.get(),
                           external_surface->bounds(),
                           external_surface->bounds().fitIn(surface->bounds()));
      surface->unlock();
    }
  }

  void PlayItem(int index) {
    playing_index_ = index % (int) input_files_.size();

    player_ = std::make_shared<media::MediaPlayer>(
        std::make_unique<media::ExternalVideoRendererSink>(),
        std::make_shared<media::AudioRendererSinkImpl>(),
        task_runner_);
    player_->OpenDataSource(input_files_[playing_index_].c_str());
    player_->SetPlayWhenReady(true);
  }

};

// static
MediaPlayerWindow *MediaPlayerWindow::instance;

void HandleAppEvent(const os::Event &ev) {
  auto appWindow = MediaPlayerWindow::instance;

  switch (ev.type()) {

    case os::Event::CloseApp:
    case os::Event::CloseWindow:MediaPlayerWindow::Close();
      break;
    case os::Event::KeyDown:appWindow->HandleKeyEvent(ev.scancode());
      break;
    default:
      // Do nothing
      break;
  }

}

void AppEventQueue() {
  auto current = media::base::MessageLooper::Current();
  DCHECK(current);

  auto *queue = os::instance()->eventQueue();
  os::Event ev;
  queue->getEvent(ev, 0.000001);
  HandleAppEvent(ev);

  current->PostDelayedTask(FROM_HERE, media::TimeDelta::FromMilliseconds(20), AppEventQueue);
}

}

namespace window {

void RefreshScreen() {
  MediaPlayerWindow::instance->Refresh();
}

}

int app_main(int argc, char *argv[]) {
  os::SystemRef system = os::make_system();
  system->setAppMode(os::AppMode::GUI);

  system->finishLaunching();

  system->activateApp();

  auto lopper = media::base::MessageLooper::Create("main", 16);
  lopper->Prepare();
  lopper->PostTask(FROM_HERE, AppEventQueue);
  lopper->PostTask(FROM_HERE, []() {
    media::MediaPlayer::GlobalInit();
    SkiaMediaTexture::RegisterTextureFactory();
  });
  lopper->PostTask(FROM_HERE, [argc, argv]() {
    std::vector<std::string> input_files(argc - 1);
    for (int i = 0; i < argc - 1; ++i) {
      input_files[i] = argv[i + 1];
    }
    DCHECK(!input_files.empty()) << "An input file must be specified.";
    MediaPlayerWindow::Initialize(std::move(input_files));
  });
  lopper->Loop();
  return 0;
}