#ifndef LYCHEE_MEDIA_FRAME_CONVERTER_H_
#define LYCHEE_MEDIA_FRAME_CONVERTER_H_

#include <cstdint>
#include <memory>

#include "media/video_frame.h"

namespace lychee {

// Turns decoder output into a storage the caller accepts.
class FrameConverter {
 public:
  virtual ~FrameConverter() = default;

  // |accepted| is a bitmask of VideoFrameStorage. Returns nullptr if the
  // conversion is not possible.
  virtual std::shared_ptr<VideoFrame> Convert(
      const std::shared_ptr<VideoFrame>& frame,
      uint32_t accepted) = 0;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_FRAME_CONVERTER_H_
