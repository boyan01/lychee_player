#ifndef LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_VIDEO_DECODER_H_
#define LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_VIDEO_DECODER_H_

#include <d3d11.h>

#include <cstdint>
#include <vector>

#include "backends/media_foundation/mf_common.h"
#include "backends/windows/d3d11_video_processor.h"
#include "media/annexb.h"
#include "media/video_decoder.h"
#include "media/video_device.h"

namespace lychee {

// H.264 and HEVC through Windows' decoder MFTs on a D3D11 device of their
// own, on the caller's adapter: Microsoft's H.264 decoder, and the HEVC
// decoder of the Store's HEVC Video Extensions where installed. Frames
// leave like FFmpeg's D3D11VA ones, as shared BGRA textures
// (D3D11VideoProcessor), or downloaded when the caller takes no textures
// or gave no adapter. Hardware only: a stream the GPU cannot decode fails
// at its first frame, and the next decoder takes over.
//
// Ported from flutter-airplay's native/backends/windows/windows_video.cpp.
class MFVideoDecoder : public VideoDecoder {
 public:
  MFVideoDecoder(uint32_t accepted_storage, VideoDevice device);
  ~MFVideoDecoder() override;

  const char* name() const override { return "mediafoundation"; }
  bool is_hardware() const override { return true; }
  bool Initialize(const VideoConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  bool CreateDevice(std::string* error);
  // Picks NV12 or P010 output, and reads its size and colors.
  bool SetOutputType();
  // Hands out decoded frames until the decoder wants input. False on
  // errors.
  bool Drain(const OutputCallback& output);
  std::shared_ptr<VideoFrame> CreateFrame(IMFSample* sample);
  bool DeviceLost();

  const VideoDevice device_;
  // Frames go out as textures only if someone can open them.
  const bool output_textures_;
  VideoConfig config_;
  AnnexBConverter converter_;
  Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  Microsoft::WRL::ComPtr<IMFTransform> decoder_;
  D3D11VideoProcessor processor_;
  // The output type's picture (its display aperture), color space, and
  // whether the decoder allocates its output samples, as it does on D3D11.
  int width_ = 0;
  int height_ = 0;
  DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  bool provides_samples_ = false;
  bool logged_output_ = false;
  int consecutive_errors_ = 0;
  std::vector<uint8_t> annexb_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_VIDEO_DECODER_H_
