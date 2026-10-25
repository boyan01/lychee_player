#ifndef LYCHEE_BACKENDS_APPLE_APPLE_FRAME_CONVERTER_H_
#define LYCHEE_BACKENDS_APPLE_APPLE_FRAME_CONVERTER_H_

#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include "media/cpu_frame_converter.h"
#include "media/frame_converter.h"

namespace lychee {

// Converts between CPU BGRA and BGRA CVPixelBuffers, and turns CVPixelBuffers
// of other formats (e.g. NV12) into BGRA with VTPixelTransferSession. CPU
// RGBA goes through CPU BGRA.
class AppleFrameConverter : public FrameConverter {
 public:
  AppleFrameConverter();
  ~AppleFrameConverter() override;

  std::shared_ptr<VideoFrame> Convert(const std::shared_ptr<VideoFrame>& frame,
                                      uint32_t accepted) override;

 private:
  CVPixelBufferRef CreateBgraBuffer(int width, int height);
  std::shared_ptr<VideoFrame> ToBgraPixelBuffer(
      const std::shared_ptr<VideoFrame>& frame);
  std::shared_ptr<VideoFrame> ToCpuBgra(
      const std::shared_ptr<VideoFrame>& frame);

  CpuFrameConverter cpu_converter_;
  VTPixelTransferSessionRef transfer_session_ = nullptr;
  CVPixelBufferPoolRef pool_ = nullptr;
  int pool_width_ = 0;
  int pool_height_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_APPLE_FRAME_CONVERTER_H_
