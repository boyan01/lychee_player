#ifndef LYCHEE_MEDIA_FRAME_SCALER_H_
#define LYCHEE_MEDIA_FRAME_SCALER_H_

#include <memory>

#include "media/video_frame.h"

namespace lychee {

// The upright size of a |width| x |height| picture with the sample aspect
// ratio |sar_num|:|sar_den| applied (to the width) and turned |rotation|
// degrees clockwise, scaled to fit |max_width| x |max_height| keeping its
// aspect ratio, never enlarged (0 is no limit). At least 1 x 1.
void FitSize(int width,
             int height,
             int sar_num,
             int sar_den,
             int rotation,
             int max_width,
             int max_height,
             int* fit_width,
             int* fit_height);

// |frame| (kCpuRgba or kCpuBgra) scaled to |width| x |height| upright
// pixels and turned |rotation| degrees clockwise (0, 90, 180, 270), in the
// same storage. A triangle filter that widens with the reduction, as
// swscale's bilinear does, so a thumbnail is not aliased. Null for other
// storage or when out of memory.
std::shared_ptr<VideoFrame> ScaleFrame(const VideoFrame& frame,
                                       int rotation,
                                       int width,
                                       int height);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_FRAME_SCALER_H_
