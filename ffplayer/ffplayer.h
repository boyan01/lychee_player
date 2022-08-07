#ifndef FFPLAYER_FFPLAYER_H_
#define FFPLAYER_FFPLAYER_H_

#include <cstdint>

struct PlayerConfiguration {
  int32_t audio_disable = false;
  int32_t video_disable = false;
  int32_t subtitle_disable = false;

  int32_t seek_by_bytes = false;

  int32_t show_status = true;

  double start_time = 0;
  int32_t loop = 1;
};

#endif  // FFPLAYER_FFPLAYER_H_