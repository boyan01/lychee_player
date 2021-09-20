//
// Created by Bin Yang on 2021/9/19.
//

#ifndef MEDIA_EXAMPLE_MEDIA_PLAYER_CONTROLLER_H_
#define MEDIA_EXAMPLE_MEDIA_PLAYER_CONTROLLER_H_

#include "os/os.h"
#include "os/skia/skia_surface.h"

#include "media_player.h"

class MediaPlayerController {

 public:

  MediaPlayerController();

  void Render(os::Surface *surface);

 private:

  std::shared_ptr<media::MediaPlayer> player_;

};

#endif //MEDIA_EXAMPLE_MEDIA_PLAYER_CONTROLLER_H_
