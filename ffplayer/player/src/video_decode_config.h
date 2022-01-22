//
// Created by yangbin on 2021/5/3.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_DECODE_CONFIG_H_
#define MEDIA_PLAYER_SRC_VIDEO_DECODE_CONFIG_H_

extern "C" {
#include "libavformat/avformat.h"
}

namespace media {

class VideoDecodeConfig {
 public:
  VideoDecodeConfig()
      : codec_parameters_(),
        time_base_(),
        frame_rate_(),
        max_frame_duration_(0) {
    memset(&codec_parameters_, 0, sizeof(AVCodecParameters));
    memset(&time_base_, 0, sizeof(AVRational));
    memset(&frame_rate_, 0, sizeof(AVRational));
  }

  VideoDecodeConfig(const AVCodecParameters &codec_parameters,
                    const AVRational time_base, const AVRational frame_rate,
                    double max_frame_duration)
      : codec_parameters_(codec_parameters),
        time_base_(time_base),
        frame_rate_(frame_rate),
        max_frame_duration_(max_frame_duration) {}

  AVCodecID codec_id() const { return codec_parameters_.codec_id; }

  const AVCodecParameters &codec_parameters() const {
    return codec_parameters_;
  }

  const AVRational &time_base() const { return time_base_; }

  const AVRational &frame_rate() const { return frame_rate_; }

  /**
   * 1 -> 1/2  2-> 1/4
   */
  int low_res() const { return 0; }

  bool fast() const { return false; }

  double max_frame_duration() const { return max_frame_duration_; }

  bool IsValidConfig() const { return true; }

 private:
  AVCodecParameters codec_parameters_;
  AVRational time_base_;
  AVRational frame_rate_;
  double max_frame_duration_;
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_VIDEO_DECODE_CONFIG_H_
