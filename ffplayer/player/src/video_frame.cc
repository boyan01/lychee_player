//
// Created by yangbin on 2021/4/19.
//

#include "video_frame.h"

namespace media {

// static
std::shared_ptr<VideoFrame> VideoFrame::CreateEmptyFrame() {
  return std::make_shared<VideoFrame>(nullptr, 0, 0, 0);
}

VideoFrame::VideoFrame(AVFrame *frame, double pts, double duration, int serial)
    : frame_(nullptr), pts_(pts), duration_(duration), serial_(serial) {
  if (frame) {
    frame_ = av_frame_alloc();
    av_frame_ref(frame_, frame);
  }
}

VideoFrame::~VideoFrame() {
  if (frame_) {
    av_frame_unref(frame_);
    av_frame_free(&frame_);
  }
}

}
