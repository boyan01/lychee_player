#ifndef LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_
#define LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <memory>

#include "media/video_frame.h"

namespace lychee {

// Turns decoded NV12 / P010 textures into kD3D11Texture frames: a new
// shared BGRA texture per frame, so a texture another device has opened is
// never written again. Runs on the decoder's device, on the decode thread.
//
// Ported from flutter-airplay's native/backends/windows/windows_gpu.h.
class D3D11VideoProcessor {
 public:
  // |texture| is a decoder output, |slice| its array index; the picture is
  // its top-left |width| x |height| in |color_space|. Null if the GPU
  // cannot convert it. Output is SDR BT.709. HDR is the driver's to map:
  // NVIDIA's maps HLG but clips PQ, so PQ frames do not come here (see
  // FFmpegVideoDecoder::CreateFrame).
  std::shared_ptr<VideoFrame> Convert(ID3D11Texture2D* texture,
                                      UINT slice,
                                      int width,
                                      int height,
                                      DXGI_COLOR_SPACE_TYPE color_space);

 private:
  bool SetUp(ID3D11Device* device);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  Microsoft::WRL::ComPtr<ID3D11VideoDevice> video_device_;
  Microsoft::WRL::ComPtr<ID3D11VideoContext> video_context_;
  // Windows 8.1 and later: DXGI color spaces, which can say PQ and HLG.
  Microsoft::WRL::ComPtr<ID3D11VideoContext1> video_context1_;
  Microsoft::WRL::ComPtr<ID3D11Query> completion_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;
  // What |processor_| was created for.
  DXGI_FORMAT input_format_ = DXGI_FORMAT_UNKNOWN;
  UINT input_width_ = 0;
  UINT input_height_ = 0;
  UINT output_width_ = 0;
  UINT output_height_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_
