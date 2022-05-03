//
// Created by boyan on 22-5-3.
//

#include "lychee_player.h"

extern "C" {
#include "SDL2/SDL.h"
#include "libavutil/log.h"
#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
}

#include <base/logging.h>

namespace lychee {

void LycheePlayer::GlobalInit() {
  av_log_set_flags(AV_LOG_SKIP_REPEATED);
  av_log_set_level(AV_LOG_INFO);
  avdevice_register_all();
  avformat_network_init();

  if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    LOG(ERROR) << "SDL fails to initialize audio subsystem! \n" << SDL_GetError();
  } else {
    DLOG(INFO) << "SDL Audio was initialized fine!";
  }
}

LycheePlayer::LycheePlayer(std::string path) {
  DLOG(INFO) << "create player: " << path;
}

LycheePlayer::~LycheePlayer() = default;

} // lychee