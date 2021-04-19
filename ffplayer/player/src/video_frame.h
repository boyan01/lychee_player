//
// Created by yangbin on 2021/4/19.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_FRAME_H_
#define MEDIA_PLAYER_SRC_VIDEO_FRAME_H_

#include "base/basictypes.h"

extern "C" {
#include "libavformat/avformat.h"
}

namespace media {

class VideoFrame {

 public:

  static VideoFrame Empty();

  VideoFrame(AVFrame *frame, double pts, double duration, int serial);

  ~VideoFrame();

  AVFrame *frame() {
    return frame_;
  }

  double pts() const {
    return pts_;
  }

  double duration() const {
    return duration_;
  }

  int serial() const {
    return serial_;
  }

 private:

  AVFrame *frame_;
  double pts_;
  double duration_;
  int serial_;

};

}

#endif //MEDIA_PLAYER_SRC_VIDEO_FRAME_H_
