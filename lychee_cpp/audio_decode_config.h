//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_DECODE_CONFIG_H_
#define MEDIA_PLAYER_SRC_AUDIO_DECODE_CONFIG_H_

extern "C" {
#include "libavformat/avformat.h"
}

namespace lychee {

class AudioDecodeConfig {
 public:
  AudioDecodeConfig() : codec_parameters_(), time_base_() {
    memset(&time_base_, 0, sizeof(AVRational));
  }

  explicit AudioDecodeConfig(const AVCodecParameters &codec_parameters,
                             AVRational time_base)
      : codec_parameters_(codec_parameters), time_base_(time_base) {}

  ~AudioDecodeConfig() = default;

  [[nodiscard]] AVCodecID CodecId() const { return codec_parameters_.codec_id; }

  [[nodiscard]] int bytes_per_channel() const { return codec_parameters_.frame_size; }

  [[nodiscard]] uint64_t channel_layout() const { return codec_parameters_.channel_layout; }

  [[nodiscard]] int channels() const { return codec_parameters_.channels; }

  [[nodiscard]] int samples_per_second() const { return codec_parameters_.sample_rate; }

  [[nodiscard]] const AVCodecParameters &codec_parameters() const {
    return codec_parameters_;
  }

  [[nodiscard]] AVRational time_base() const { return time_base_; }

  [[nodiscard]] bool IsValidConfig() const { return true; }

 private:
  AVCodecParameters codec_parameters_;
  AVRational time_base_;
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_AUDIO_DECODE_CONFIG_H_
