#ifndef LYCHEE_BACKENDS_APPLE_PIXEL_BUFFER_H_
#define LYCHEE_BACKENDS_APPLE_PIXEL_BUFFER_H_

#include <memory>

#include "media/video_frame.h"

namespace lychee {

struct ColorInfo;

// Retains |pixel_buffer| (a CVPixelBufferRef) for the frame's lifetime. A
// YCbCr buffer without a matrix gets the library's convention attached:
// BT.709 above 576 lines, BT.601 otherwise. With source color information,
// its unspecified fields take the defaults rather than decoder guesses.
std::shared_ptr<VideoFrame> WrapPixelBuffer(void* pixel_buffer,
                                            const ColorInfo* color = nullptr);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_PIXEL_BUFFER_H_
