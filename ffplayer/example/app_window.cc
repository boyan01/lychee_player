//
// Created by Bin Yang on 2021/9/20.
//

#include "app_window.h"

#include "base/logging.h"

AppWindow *AppWindow::app_window_instance_ = nullptr;

AppWindow *AppWindow::Instance() {
  DCHECK(app_window_instance_) << "didn't initialized.";
  return app_window_instance_;
}

void AppWindow::Initialize() {
  DCHECK(!app_window_instance_);
  app_window_instance_ = new AppWindow();
}

AppWindow::AppWindow() {
  uint32_t flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;

  auto ret = SDL_Init(flags);
  DCHECK_EQ(ret, 0) << "Could not initialize SDL: " << SDL_GetError();

  SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
  SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

  auto window_flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
  window_ = SDL_CreateWindow("Lychee", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, 640, 480, window_flags);
  DCHECK(window_) << "failed to create window." << SDL_GetError();

  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

  renderer_ = std::shared_ptr<SDL_Renderer>(
      SDL_CreateRenderer(window_, -1,
                         SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC),
      [](SDL_Renderer *ptr) { SDL_DestroyRenderer(ptr); });
  if (!renderer_) {
    DLOG(WARNING) << "failed to initialize a hardware accelerated GetRenderer: "
                  << SDL_GetError();
    renderer_ = std::shared_ptr<SDL_Renderer>(
        SDL_CreateRenderer(window_, -1, 0),
        [](SDL_Renderer *ptr) { SDL_DestroyRenderer(ptr); });
  }
  DCHECK(renderer_) << "failed to create GetRenderer: " << SDL_GetError();

  SDL_RendererInfo renderer_info = {nullptr};
  if (!SDL_GetRendererInfo(renderer_.get(), &renderer_info)) {
    DLOG(INFO) << "Initialized " << renderer_info.name << " GetRenderer.";
  }

  DCHECK_GT(renderer_info.num_texture_formats, 0)
      << "Failed to create window or GetRenderer: " << SDL_GetError();

  SDL_ShowWindow(window_);
}
