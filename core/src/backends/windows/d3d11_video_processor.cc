#define LOG_TAG "d3d11"

#include "backends/windows/d3d11_video_processor.h"

#include <d3dcompiler.h>

#include <thread>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

using Microsoft::WRL::ComPtr;

// Longest wait for the GPU to finish a conversion.
constexpr int64_t kCompletionTimeout = 100 * kMicrosPerMilli;

// PQ into SDR close to how swscale does it for software-decoded frames
// (FFmpegVideoDecoder::ConvertToCpu), so a stream looks about the same
// whichever decoder runs. swscale's default intent, relative colorimetric,
// keeps absolute luminance with 203 nits as SDR white. Measured with
// FFmpeg 9.0.2: a 0.5 PQ gray (~90 nits) reads 181, the 100-nit bar of
// hdr_pq_bars.mp4 190, 203 nits 254, brighter bars 255.
//  - m1, m2, c1, c2, c3: the PQ EOTF and its inverse (SMPTE ST 2084).
//  - 203 nits: SDR reference white (ITU-R BT.2408), mapped to 1.0.
//  - Colors outside BT.709 or brighter than white move in ICtCp (ITU-R
//    BT.2100; the BT.2020-to-LMS and LMS-to-ICtCp matrices are its
//    integer ones over 4096) along a line that keeps hue, toward a gray of
//    half their intensity (capped at white's), to the gamut's edge. swscale
//    clips in IPT along similar lines; its exact result is not reproduced.
//    Fitted on 84 colors (12 hues, 20 to 4000 nits) against swscale: grays
//    and in-gamut colors match within 1, skin, sky and pastel tones within
//    9, saturated primaries at or above white within 67 (median 11; a
//    per-channel clip gives median 19, 90th percentile 129, and turns
//    bright reds white). The half-intensity anchor scored best among 0.3
//    to 1.0.
//  - kLmsToBt709: BT.2100's LMS back to BT.2020 RGB, then BT.2087's
//    BT.2020-to-BT.709 matrix, multiplied out.
//  - 1/2.4: the inverse of BT.1886 with a zero black level, how swscale
//    encodes BT.709 output.
constexpr char kToneMapShader[] = R"(
Texture2D<float4> pq : register(t0);

float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

static const float m1 = 2610.0 / 16384.0;
static const float m2 = 2523.0 / 4096.0 * 128.0;
static const float c1 = 3424.0 / 4096.0;
static const float c2 = 2413.0 / 4096.0 * 32.0;
static const float c3 = 2392.0 / 4096.0 * 32.0;
static const float kWhiteNits = 203.0;
// PQ of 203 nits.
static const float kWhiteIntensity = 0.580689;
static const float3x3 kBt2020ToLms = float3x3(
  1688.0, 2146.0,  262.0,
   683.0, 2951.0,  462.0,
    99.0,  309.0, 3688.0) / 4096.0;
static const float3x3 kLmsToIctcp = float3x3(
   2048.0,   2048.0,    0.0,
   6610.0, -13613.0, 7003.0,
  17933.0, -17390.0, -543.0) / 4096.0;
static const float3x3 kIctcpToLms = {
  1.0,  0.008609,  0.111030,
  1.0, -0.008609, -0.111030,
  1.0,  0.560031, -0.320627,
};
static const float3x3 kLmsToBt709 = {
   6.17336, -5.32033,  0.14707,
  -1.32448,  2.56035, -0.23586,
  -0.01197, -0.26459,  1.27646,
};

float3 PqToNits(float3 e) {
  float3 p = pow(saturate(e), 1.0 / m2);
  return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

float3 NitsToPq(float3 nits) {
  float3 y = pow(max(nits, 0.0) / 10000.0, m1);
  return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

// Linear BT.709, 1.0 at white.
float3 IctcpToSdr(float3 ictcp) {
  return mul(kLmsToBt709, PqToNits(mul(kIctcpToLms, ictcp))) / kWhiteNits;
}

bool Inside(float3 rgb) {
  return all(rgb >= -1e-4) && all(rgb <= 1.0 + 1e-4);
}

float4 ps(float4 position : SV_Position) : SV_Target {
  float3 nits = PqToNits(pq.Load(int3(position.xy, 0)).rgb);
  float3 ictcp = mul(kLmsToIctcp, NitsToPq(mul(kBt2020ToLms, nits)));
  float3 rgb = IctcpToSdr(ictcp);
  [branch] if (!Inside(rgb)) {
    float anchor = 0.5 * min(ictcp.x, kWhiteIntensity);
    float lo = 0.0;
    float hi = 1.0;
    [unroll] for (int i = 0; i < 12; ++i) {
      float t = 0.5 * (lo + hi);
      if (Inside(IctcpToSdr(float3(lerp(anchor, ictcp.x, t), t * ictcp.yz)))) {
        lo = t;
      } else {
        hi = t;
      }
    }
    rgb = IctcpToSdr(float3(lerp(anchor, ictcp.x, lo), lo * ictcp.yz));
  }
  return float4(pow(saturate(rgb), 1.0 / 2.4), 1.0);
}
)";

bool IsPq(DXGI_COLOR_SPACE_TYPE color_space) {
  return color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ||
         color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
}

ComPtr<ID3DBlob> CompileShader(const char* entry, const char* target) {
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  if (FAILED(D3DCompile(kToneMapShader, sizeof(kToneMapShader) - 1, "tone_map",
                        nullptr, nullptr, entry, target,
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
    LOGE("cannot compile the tone-mapping shader: %s",
         errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
    return nullptr;
  }
  return code;
}

}  // namespace

DXGI_COLOR_SPACE_TYPE DxgiColorSpace(const ColorInfo& color, int height) {
  // H.273: transfer 16 is PQ, 18 HLG; matrix 9 and 10 BT.2020, 5 and 6
  // BT.601, 2 unspecified.
  const bool full = color.full_range;
  if (color.transfer == 16) {
    return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
  }
  if (color.transfer == 18) {
    return DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
  }
  if (color.matrix == 9 || color.matrix == 10) {
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
  }
  const bool bt709 = color.matrix == 2 ? height > 576
                                       : color.matrix != 5 && color.matrix != 6;
  if (bt709) {
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  }
  return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
              : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
}

bool D3D11VideoProcessor::SetUp(ID3D11Device* device) {
  if (device == device_.Get()) {
    return video_device_ && video_context_ && completion_;
  }
  *this = D3D11VideoProcessor();
  device_ = device;
  device_->GetImmediateContext(&context_);
  D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
  context_.As(&video_context1_);
  return SUCCEEDED(device_.As(&video_device_)) &&
         SUCCEEDED(context_.As(&video_context_)) &&
         SUCCEEDED(device_->CreateQuery(&query, &completion_));
}

std::shared_ptr<VideoFrame> D3D11VideoProcessor::Convert(
    ID3D11Texture2D* texture,
    UINT slice,
    int width,
    int height,
    DXGI_COLOR_SPACE_TYPE color_space) {
  ComPtr<ID3D11Device> device;
  texture->GetDevice(&device);
  if (!SetUp(device.Get()) || FAILED(device_->GetDeviceRemovedReason())) {
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC source = {};
  texture->GetDesc(&source);
  const UINT output_width = static_cast<UINT>(width);
  const UINT output_height = static_cast<UINT>(height);
  if (width <= 0 || height <= 0 || slice >= source.ArraySize ||
      output_width > source.Width || output_height > source.Height ||
      (source.Format != DXGI_FORMAT_NV12 &&
       source.Format != DXGI_FORMAT_P010)) {
    return nullptr;
  }
  // PQ needs DXGI color spaces to come out of the video processor as PQ.
  const bool pq = IsPq(color_space);
  if (pq && (!video_context1_ || shader_failed_)) {
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC target = {};
  target.Width = output_width;
  target.Height = output_height;
  target.MipLevels = target.ArraySize = 1;
  target.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  target.SampleDesc.Count = 1;
  target.Usage = D3D11_USAGE_DEFAULT;
  target.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  target.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  ComPtr<ID3D11Texture2D> output;
  if (FAILED(device_->CreateTexture2D(&target, nullptr, &output))) {
    return nullptr;
  }

  if (pq) {
    D3D11_TEXTURE2D_DESC desc = {};
    if (pq_) {
      pq_->GetDesc(&desc);
    }
    if (!pq_ || desc.Width != output_width || desc.Height != output_height) {
      pq_.Reset();
      pq_view_.Reset();
      desc = target;
      // 10 bits of PQ, as P010 carries them.
      desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      desc.MiscFlags = 0;
      if (FAILED(device_->CreateTexture2D(&desc, nullptr, &pq_)) ||
          FAILED(device_->CreateShaderResourceView(pq_.Get(), nullptr,
                                                   &pq_view_))) {
        pq_.Reset();
        return nullptr;
      }
    }
    if (!Blt(texture, slice, source.Format, source.Width, source.Height,
             pq_.Get(), output_width, output_height, color_space,
             DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ||
        !ToneMapPq(output.Get(), output_width, output_height)) {
      return nullptr;
    }
  } else if (!Blt(texture, slice, source.Format, source.Width, source.Height,
                  output.Get(), output_width, output_height, color_space,
                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)) {
    return nullptr;
  }
  if (!Finish()) {
    return nullptr;
  }

  auto frame = std::make_shared<VideoFrame>();
  frame->storage = VideoFrameStorage::kD3D11Texture;
  frame->width = width;
  frame->height = height;
  frame->native_handle = output.Get();
  frame->owner = std::shared_ptr<ID3D11Texture2D>(
      output.Detach(), [](ID3D11Texture2D* texture) { texture->Release(); });
  return frame;
}

bool D3D11VideoProcessor::Blt(ID3D11Texture2D* texture,
                              UINT slice,
                              DXGI_FORMAT format,
                              UINT input_width,
                              UINT input_height,
                              ID3D11Texture2D* output,
                              UINT output_width,
                              UINT output_height,
                              DXGI_COLOR_SPACE_TYPE input_space,
                              DXGI_COLOR_SPACE_TYPE output_space) {
  if (!processor_ || format != input_format_ || input_width != input_width_ ||
      input_height != input_height_ || output_width != output_width_ ||
      output_height != output_height_) {
    processor_.Reset();
    enumerator_.Reset();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = content.OutputFrameRate = {60, 1};
    content.InputWidth = input_width;
    content.InputHeight = input_height;
    content.OutputWidth = output_width;
    content.OutputHeight = output_height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    UINT input_support = 0;
    UINT output_support = 0;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content,
                                                             &enumerator_)) ||
        FAILED(enumerator_->CheckVideoProcessorFormat(format,
                                                      &input_support)) ||
        !(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        FAILED(enumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &output_support)) ||
        !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ||
        FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                                   &processor_))) {
      LOGW("no video processor for format %d %ux%u", format, input_width,
           input_height);
      processor_.Reset();
      return false;
    }
    input_format_ = format;
    input_width_ = input_width;
    input_height_ = input_height;
    output_width_ = output_width;
    output_height_ = output_height;
  }

  D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
  input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
  input_desc.Texture2D.ArraySlice = slice;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
  output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11VideoProcessorInputView> input_view;
  ComPtr<ID3D11VideoProcessorOutputView> output_view;
  if (FAILED(video_device_->CreateVideoProcessorInputView(
          texture, enumerator_.Get(), &input_desc, &input_view)) ||
      FAILED(video_device_->CreateVideoProcessorOutputView(
          output, enumerator_.Get(), &output_desc, &output_view))) {
    return false;
  }

  const RECT rect = {0, 0, static_cast<LONG>(output_width),
                     static_cast<LONG>(output_height)};
  ID3D11VideoProcessor* processor = processor_.Get();
  video_context_->VideoProcessorSetStreamFrameFormat(
      processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
  video_context_->VideoProcessorSetStreamSourceRect(processor, 0, TRUE, &rect);
  video_context_->VideoProcessorSetStreamDestRect(processor, 0, TRUE, &rect);
  video_context_->VideoProcessorSetOutputTargetRect(processor, TRUE, &rect);
  video_context_->VideoProcessorSetStreamAutoProcessingMode(processor, 0,
                                                            FALSE);
  if (video_context1_) {
    video_context1_->VideoProcessorSetStreamColorSpace1(processor, 0,
                                                        input_space);
    video_context1_->VideoProcessorSetOutputColorSpace1(processor,
                                                        output_space);
  } else {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color = {};
    input_color.YCbCr_Matrix =
        input_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 ||
                input_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
            ? 0
            : 1;
    input_color.Nominal_Range =
        input_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 ||
                input_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 ||
                input_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
            ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
            : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color = {};
    video_context_->VideoProcessorSetStreamColorSpace(processor, 0,
                                                      &input_color);
    video_context_->VideoProcessorSetOutputColorSpace(processor,
                                                      &output_color);
  }
  video_context_->VideoProcessorSetOutputAlphaFillMode(
      processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view.Get();
  return SUCCEEDED(video_context_->VideoProcessorBlt(
      processor, output_view.Get(), 0, 1, &stream));
}

bool D3D11VideoProcessor::ToneMapPq(ID3D11Texture2D* output,
                                    UINT width,
                                    UINT height) {
  if (!tone_map_shader_) {
    ComPtr<ID3DBlob> vertex = CompileShader("vs", "vs_4_0");
    ComPtr<ID3DBlob> pixel = CompileShader("ps", "ps_4_0");
    if (!vertex || !pixel ||
        FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(),
                                           vertex->GetBufferSize(), nullptr,
                                           &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(pixel->GetBufferPointer(),
                                          pixel->GetBufferSize(), nullptr,
                                          &tone_map_shader_))) {
      shader_failed_ = true;
      vertex_shader_.Reset();
      tone_map_shader_.Reset();
      return false;
    }
  }
  ComPtr<ID3D11RenderTargetView> target;
  if (FAILED(device_->CreateRenderTargetView(output, nullptr, &target))) {
    return false;
  }
  // The device is the decoder's own; nothing else draws with it, so the
  // state is set without saving what was there.
  const D3D11_VIEWPORT viewport = {0, 0, static_cast<float>(width),
                                   static_cast<float>(height), 0, 1};
  ID3D11RenderTargetView* targets[] = {target.Get()};
  ID3D11ShaderResourceView* views[] = {pq_view_.Get()};
  context_->OMSetRenderTargets(1, targets, nullptr);
  context_->RSSetViewports(1, &viewport);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(tone_map_shader_.Get(), nullptr, 0);
  context_->PSSetShaderResources(0, 1, views);
  // One triangle over the whole target (vs above).
  context_->Draw(3, 0);
  ID3D11ShaderResourceView* no_views[] = {nullptr};
  context_->PSSetShaderResources(0, 1, no_views);
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  return true;
}

bool D3D11VideoProcessor::Finish() {
  // Flush only submits the work: wait for it before another device reads
  // the shared texture, but never indefinitely.
  context_->End(completion_.Get());
  context_->Flush();
  const int64_t deadline = MonotonicMicros() + kCompletionTimeout;
  HRESULT status;
  while ((status = context_->GetData(completion_.Get(), nullptr, 0,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH)) ==
         S_FALSE) {
    if (MonotonicMicros() >= deadline) {
      LOGW("video processor timed out");
      return false;
    }
    std::this_thread::yield();
  }
  return SUCCEEDED(status);
}

}  // namespace lychee
