#include "os/os.h"

#include "media_player.h"
#include "media_player_controller.h"

#include "skia_media_texture.h"

void draw_window(os::Window *window) {

  static auto view = new MediaPlayerController();

  // Invalidates the whole window to show it on the screen.
  if (window->isVisible())
    window->invalidate();
  else
    window->setVisible(true);

  view->Render(window->surface());

}

int app_main(int argc, char *argv[]) {
  os::SystemRef system = os::make_system();
  system->setAppMode(os::AppMode::GUI);

  os::WindowRef window = os::instance()->makeWindow(400, 300);
  window->setTitle("Hello World");
  window->setCursor(os::NativeCursor::Arrow);

  system->handleWindowResize = draw_window;

  system->finishLaunching();

  system->activateApp();

  media::MediaPlayer::GlobalInit();
  SkiaMediaTexture::RegisterTextureFactory();

  // Wait until a key is pressed or the window is closed

  os::EventQueue *queue = system->eventQueue();
  bool running = true;
  bool redraw = true;
  while (running) {
    // TODO use TaskRunner to handle event.
    draw_window(window.get());
    // Wait for an event in the queue, the "true" parameter indicates
    // that we'll wait for a new event, and the next line will not be
    // processed until we receive a new event. If we use "false" and
    // there is no events in the queue, we receive an "ev.type() == Event::None
    os::Event ev;
    queue->getEvent(ev, 0.010);

    switch (ev.type()) {

      case os::Event::CloseApp:
      case os::Event::CloseWindow:running = false;
        break;

      case os::Event::KeyDown:
        switch (ev.scancode()) {
          case os::kKeyEsc:running = false;
            break;
          case os::kKey1:
          case os::kKey2:
          case os::kKey3:
          case os::kKey4:
          case os::kKey5:
          case os::kKey6:
          case os::kKey7:
          case os::kKey8:
          case os::kKey9:
            // Set scale
            window->setScale(1 + (int) (ev.scancode() - os::kKey1));
            redraw = true;
            break;

          case os::kKeyF:
          case os::kKeyF11:window->setFullscreen(!window->isFullscreen());
            break;

          default:
            // Do nothing
            break;
        }
        break;

      case os::Event::ResizeWindow:redraw = true;
        break;

      default:
        // Do nothing
        break;
    }
  }

  return 0;
}