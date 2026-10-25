#ifndef LYCHEE_MEDIA_CPU_FRAME_CONVERTER_H_
#define LYCHEE_MEDIA_CPU_FRAME_CONVERTER_H_

#include "media/frame_converter.h"

namespace lychee {

// Converts between the CPU frame types (BGRA and RGBA) by swapping the red
// and blue channels. The converter on platforms without a native one, and
// the last step of the others.
class CpuFrameConverter : public FrameConverter {
 public:
  std::shared_ptr<VideoFrame> Convert(const std::shared_ptr<VideoFrame>& frame,
                                      uint32_t accepted) override;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_CPU_FRAME_CONVERTER_H_
