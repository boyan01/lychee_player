#ifndef LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_
#define LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_

#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <memory>

#include "media/stream_info.h"
#include "media/video_frame.h"

namespace lychee {

// The DXGI color space of YCbCr video with |color| (H.273 code points). An
// unspecified matrix is BT.709 above 576 lines, BT.601 otherwise.
DXGI_COLOR_SPACE_TYPE DxgiColorSpace(const ColorInfo& color, int height);

// Turns decoded NV12 / P010 textures into shared BGRA or PQ/BT.2020 R10
// frames: a new texture per frame, so a texture another device has opened is
// never written again. Runs on the decoder's device, on the decode thread.
//
// Ported from flutter-airplay's native/backends/windows/windows_gpu.h.
class D3D11VideoProcessor {
 public:
  // |texture| is a decoder output, |slice| its array index; the picture is
  // its top-left |width| x |height| in |color_space|. Null if the GPU
  // cannot convert it. By default output is SDR BT.709. HDR (PQ, which NVIDIA's
  // video processor clips to white, and HLG, which it maps darker than swscale)
  // goes through a shader of our own. With |hdr_output|, HDR leaves as
  // full-range PQ / BT.2020 in R10G10B10A2; HLG uses a 1000-nit reference
  // display. SDR keeps the ordinary BGRA type.
  std::shared_ptr<VideoFrame> Convert(ID3D11Texture2D* texture,
                                      UINT slice,
                                      int width,
                                      int height,
                                      DXGI_COLOR_SPACE_TYPE color_space,
                                      bool hdr_output = false);

  // A kCpuBgra copy of |frame|, a frame from Convert(), for callers that
  // cannot open textures. Null on failure.
  std::shared_ptr<VideoFrame> Download(const VideoFrame& frame);

 private:
  bool SetUp(ID3D11Device* device);
  // Converts |texture|'s |slice| into |output| from |input_space| to
  // |output_space|; the video processor matches |format| and the sizes.
  bool Blt(ID3D11Texture2D* texture,
           UINT slice,
           DXGI_FORMAT format,
           UINT input_width,
           UINT input_height,
           ID3D11Texture2D* output,
           UINT output_width,
           UINT output_height,
           DXGI_COLOR_SPACE_TYPE input_space,
           DXGI_COLOR_SPACE_TYPE output_space);
  // Converts |hdr_|, PQ or HLG / BT.2020, to SDR or native PQ output.
  bool ConvertHdr(ID3D11Texture2D* output,
                  UINT width,
                  UINT height,
                  bool hlg,
                  bool hdr_output);
  // Waits for the GPU to finish what was submitted.
  bool Finish();

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
  DXGI_FORMAT output_format_ = DXGI_FORMAT_UNKNOWN;
  UINT input_width_ = 0;
  UINT input_height_ = 0;
  UINT output_width_ = 0;
  UINT output_height_ = 0;
  // HDR: the video processor's RGB output, still in PQ or HLG, and the
  // shaders that read it. Created on the first HDR frame.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> hdr_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hdr_view_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> pq_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> hlg_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> hlg_pq_shader_;
  bool shader_failed_ = false;
  // Download(): reused while the size stays.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_
