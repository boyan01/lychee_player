//
// Created by yangbin on 2021/4/19.
//

#include "video_frame.h"

namespace media {

// static
VideoFrame VideoFrame::Empty() {
  return VideoFrame(nullptr, 0, 0, 0);
}

VideoFrame::VideoFrame(AVFrame *frame, double pts, double duration, int serial)
    : frame_(frame), pts_(pts), duration_(duration), serial_(serial) {}


VideoFrame::~VideoFrame() = default;

}
