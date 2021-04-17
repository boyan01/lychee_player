//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_DECODER_VIDEO_FRAME_H_
#define MEDIA_PLAYER_DECODER_VIDEO_FRAME_H_

#include <memory>
#include <functional>

#include "base/basictypes.h"
#include "base/rect.h"
#include "base/timestamps.h"

extern "C" {
#include "libavformat/avformat.h"
};

namespace media {

class VideoFrame {

 public:

  static std::shared_ptr<VideoFrame> Create(AVFrame *av_frame, const AVRational &time_base);

  // Public cause make_shared() need.
  VideoFrame(AVFrame *frame, const TimeDelta &pts, const TimeDelta &duration);

  ~VideoFrame();

  AVFrame *GetFrame() const;

  const TimeDelta &GetPts() const;

  const TimeDelta &GetDuration() const;

  int GetWidth() const;

  int GetHeight() const;

  AVPixelFormat GetPixelFormat() const;

 private:

  AVFrame *frame_;

  TimeDelta pts_;

  TimeDelta duration_;

  int width_;
  int height_;

  AVPixelFormat pixel_format_;

  DISALLOW_IMPLICIT_CONSTRUCTORS(VideoFrame);

};

} // namespace media



#endif // MEDIA_PLAYER_DECODER_VIDEO_FRAME_H_
