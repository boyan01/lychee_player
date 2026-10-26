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

// Turns decoded NV12 / P010 textures into kD3D11Texture frames: a new
// shared BGRA texture per frame, so a texture another device has opened is
// never written again. Runs on the decoder's device, on the decode thread.
//
// Ported from flutter-airplay's native/backends/windows/windows_gpu.h.
class D3D11VideoProcessor {
 public:
  // |texture| is a decoder output, |slice| its array index; the picture is
  // its top-left |width| x |height| in |color_space|. Null if the GPU
  // cannot convert it. Output is SDR BT.709. HLG is the video processor's
  // to map; PQ, which NVIDIA's clips to white, goes through a shader of our
  // own (ToneMapPq).
  std::shared_ptr<VideoFrame> Convert(ID3D11Texture2D* texture,
                                      UINT slice,
                                      int width,
                                      int height,
                                      DXGI_COLOR_SPACE_TYPE color_space);

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
  // Tone-maps |pq_|, RGB in PQ with BT.2020 primaries, into |output|.
  bool ToneMapPq(ID3D11Texture2D* output, UINT width, UINT height);
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
  UINT input_width_ = 0;
  UINT input_height_ = 0;
  UINT output_width_ = 0;
  UINT output_height_ = 0;
  // PQ: the video processor's RGB output, still in PQ, and the shader that
  // reads it. Created on the first PQ frame.
  Microsoft::WRL::ComPtr<ID3D11Texture2D> pq_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pq_view_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> tone_map_shader_;
  bool shader_failed_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_WINDOWS_D3D11_VIDEO_PROCESSOR_H_
