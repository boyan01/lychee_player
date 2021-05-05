//
// Created by yangbin on 2021/5/5.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_DEVICE_INFO_H_
#define MEDIA_PLAYER_SRC_AUDIO_DEVICE_INFO_H_

#include "base/basictypes.h"

extern "C" {
#include "libavformat/avformat.h"
}

namespace media {

struct AudioDeviceInfo {
  // sample rate.
  int freq;
  int channels;
  int64_t channel_layout;
  enum AVSampleFormat fmt;
  int frame_size;
  int bytes_per_sec;
};

}

#endif //MEDIA_PLAYER_SRC_AUDIO_DEVICE_INFO_H_
