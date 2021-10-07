//
// Created by Bin Yang on 2021/9/20.
//

#ifndef MEDIA_EXAMPLE_APP_WINDOW_H_
#define MEDIA_EXAMPLE_APP_WINDOW_H_

extern "C" {
#include "SDL2/SDL.h"
}

class AppWindow {

 public:

  static AppWindow *Instance();

  static void Initialize();

  const std::shared_ptr<SDL_Renderer> &GetRenderer() {
    return renderer_;
  }

  SDL_Window *GetWindow() {
    return window_;
  }

 private:

  AppWindow();

  static AppWindow *app_window_instance_;

  SDL_Window *window_;
  std::shared_ptr<SDL_Renderer> renderer_;

};

#endif //MEDIA_EXAMPLE_APP_WINDOW_H_
