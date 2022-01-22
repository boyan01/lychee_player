//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_DECODE_CONFIG_H_
#define MEDIA_PLAYER_SRC_AUDIO_DECODE_CONFIG_H_

extern "C" {
#include "libavformat/avformat.h"
}

namespace media {

class AudioDecodeConfig {
 public:
  AudioDecodeConfig() : codec_parameters_(), time_base_() {
    memset(&time_base_, 0, sizeof(AVRational));
  }

  explicit AudioDecodeConfig(const AVCodecParameters &codec_parameters,
                             AVRational time_base)
      : codec_parameters_(codec_parameters), time_base_(time_base) {}

  ~AudioDecodeConfig() {}

  AVCodecID CodecId() const { return codec_parameters_.codec_id; }

  int bytes_per_channel() const { return codec_parameters_.frame_size; }

  uint64_t channel_layout() const { return codec_parameters_.channel_layout; }

  int channels() const { return codec_parameters_.channels; }

  int samples_per_second() const { return codec_parameters_.sample_rate; }

  const AVCodecParameters &codec_parameters() const {
    return codec_parameters_;
  }

  AVRational time_base() const { return time_base_; }

  bool IsValidConfig() const { return true; }

 private:
  AVCodecParameters codec_parameters_;
  AVRational time_base_;
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_AUDIO_DECODE_CONFIG_H_
