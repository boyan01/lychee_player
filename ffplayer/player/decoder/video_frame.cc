//
// Created by yangbin on 2021/3/28.
//

#include "base/logging.h"

#include "decoder/video_frame.h"
#include "ffmpeg/ffmpeg_utils.h"

namespace media {

// static
std::shared_ptr<VideoFrame> VideoFrame::Create(AVFrame *av_frame, const AVRational &time_base) {
  auto pts = ffmpeg::ConvertFromTimeBase(time_base, av_frame->pts);
  auto duration = ffmpeg::ConvertFromTimeBase(time_base, av_frame->pkt_duration);
  return std::make_shared<VideoFrame>(av_frame, pts, duration);
}

VideoFrame::VideoFrame(AVFrame *frame, const TimeDelta &pts, const TimeDelta &duration)
    : frame_(frame), pts_(pts), duration_(duration),
      width_(frame->width), height_(frame->height),
      pixel_format_(static_cast<AVPixelFormat>(frame->format)) {

}

VideoFrame::~VideoFrame() {
  av_frame_unref(frame_);
}

AVFrame *VideoFrame::GetFrame() const {
  return frame_;
}

const TimeDelta &VideoFrame::GetPts() const {
  return pts_;
}

const TimeDelta &VideoFrame::GetDuration() const {
  return duration_;
}

int VideoFrame::GetWidth() const {
  return width_;
}

int VideoFrame::GetHeight() const {
  return height_;
}

AVPixelFormat VideoFrame::GetPixelFormat() const {
  return pixel_format_;
}

} // namespace media