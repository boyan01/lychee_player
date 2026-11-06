#define LOG_TAG "d3d11"

#include "backends/windows/d3d11_video_processor.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

using Microsoft::WRL::ComPtr;

// Longest wait for the GPU to finish a conversion.
constexpr int64_t kCompletionTimeout = 100 * kMicrosPerMilli;
// Without a fence: how long Finish() polls the GPU between yields before
// it sleeps instead (a Sleep(1) may take a whole 15.6 ms timer tick). A
// conversion usually takes about a millisecond.
constexpr int64_t kCompletionSpin = 2 * kMicrosPerMilli;
// How long an output texture whose frame was handed out stays untouched
// after its release before it is written again. The caller releases a
// frame once it has submitted its last draw from it, which its device may
// run a few frames later (DXGI queues up to three by default): 100 ms
// covers that at refresh rates down to 30 Hz. Frames never handed out (the
// renderer drops them, or Download() read them) are reused at once: after
// a seek the decoder runs through hundreds a second.
constexpr int64_t kReuseDelay = 100 * kMicrosPerMilli;
// Output textures idle this long are more than the decoder needs; they are
// freed rather than kept.
constexpr int64_t kIdleLimit = kMicrosPerSecond;
// Decoders cycle through a fixed set of textures (FFmpeg's pool of array
// slices); more than this many input views means they changed.
constexpr size_t kMaxInputViews = 64;
// Download() buffers kept idle: more than the renderer's queue and the
// frame on screen ever return at once.
constexpr size_t kMaxIdlePixels = 8;

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
// HLG becomes light the same way swscale's does, then follows PQ's path:
//  - a, b, c: the HLG inverse OETF (ITU-R BT.2100).
//  - The OOTF for a 1000-nit display: system gamma 1.2 on BT.2020
//    luminance, no black lift. HLG's 75% (BT.2408's reference white) is
//    then 203 nits, and a 0.5 gray reads 142 as with swscale.
constexpr char kHdrShader[] = R"(
Texture2D<float4> hdr : register(t0);

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

static const float a = 0.17883277;
static const float b = 0.28466892;
static const float c = 0.55991073;
static const float kHlgPeakNits = 1000.0;
static const float kHlgGamma = 1.2;

float3 HlgToNits(float3 e) {
  e = saturate(e);
  float3 scene = e <= 0.5 ? e * e / 3.0 : (exp((e - c) / a) + b) / 12.0;
  float y = dot(scene, float3(0.2627, 0.6780, 0.0593));
  return kHlgPeakNits * pow(max(y, 1e-6), kHlgGamma - 1.0) * scene;
}

// Light in nits, BT.2020, into SDR BT.709.
float4 NitsToSdr(float3 nits) {
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

float4 ps_pq(float4 position : SV_Position) : SV_Target {
  return NitsToSdr(PqToNits(hdr.Load(int3(position.xy, 0)).rgb));
}

float4 ps_hlg(float4 position : SV_Position) : SV_Target {
  return NitsToSdr(HlgToNits(hdr.Load(int3(position.xy, 0)).rgb));
}

float4 ps_hlg_pq(float4 position : SV_Position) : SV_Target {
  return float4(NitsToPq(HlgToNits(hdr.Load(int3(position.xy, 0)).rgb)), 1.0);
}
)";

// Sample each chroma texel at the center of its 2x2 luma block. Some
// video processors instead reconstruct it one luma line below that point,
// which shifts saturated edges even when no scaling is requested.
constexpr char kNv12Shader[] = R"(
Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
SamplerState linear_clamp : register(s0);
cbuffer Conversion : register(b0) {
  float4 coefficients;
  float4 levels;
  float4 dimensions;
};

float4 ps_nv12(float4 position : SV_Position) : SV_Target {
  float y = (luma.Load(int3(position.xy, 0)) - levels.x) * levels.y;
  // Decoder padding is outside the visible image. Clamp to the center
  // of the final visible UV texel before applying bilinear filtering.
  float2 edge = (2.0 * ceil(dimensions.zw * 0.5) - 1.0) / dimensions.xy;
  float2 coordinate = min(position.xy / dimensions.xy, edge);
  float2 uv = (chroma.SampleLevel(linear_clamp, coordinate, 0) - levels.ww) * levels.z;
  return float4(y + coefficients.x * uv.y,
                y + coefficients.y * uv.x + coefficients.z * uv.y,
                y + coefficients.w * uv.x, 1.0);
}
)";

bool IsSdr601Or709(DXGI_COLOR_SPACE_TYPE color_space) {
  return color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 ||
         color_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 ||
         color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 ||
         color_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
}

bool IsPq(DXGI_COLOR_SPACE_TYPE color_space) {
  return color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ||
         color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;
}

bool IsHlg(DXGI_COLOR_SPACE_TYPE color_space) {
  return color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020 ||
         color_space == DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020;
}

// |object|'s reference count.
ULONG References(IUnknown* object) {
  object->AddRef();
  return object->Release();
}

ComPtr<ID3DBlob> CompileShader(const char* entry,
                               const char* target,
                               const char* source = kHdrShader) {
  ComPtr<ID3DBlob> code;
  ComPtr<ID3DBlob> errors;
  if (FAILED(D3DCompile(source, std::strlen(source), "frame_conversion",
                        nullptr, nullptr, entry, target,
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
    LOGE("cannot compile the frame shader: %s",
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
  const bool bt709 =
      color.matrix == 2 ? height > 576 : color.matrix != 5 && color.matrix != 6;
  if (bt709) {
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  }
  return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
              : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
}

struct D3D11VideoProcessor::Output {
  ComPtr<ID3D11Texture2D> texture;
  // The video processor output view, for |enumerator|.
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
  ComPtr<ID3D11VideoProcessorOutputView> view;
  // For the HDR shader.
  ComPtr<ID3D11RenderTargetView> target;
  // |texture|'s reference count when its frame went out: more when it
  // comes back means a caller still holds it.
  ULONG references = 0;
  // When it may be written again (MonotonicMicros()).
  int64_t reusable = 0;
  // Whether its frame was handed out (VideoFrame::present).
  std::atomic<bool> shown{false};
};

struct D3D11VideoProcessor::OutputPool {
  std::mutex mutex;
  // What the idle textures are; a texture of another size or format is
  // freed on its return.
  UINT width = 0;
  UINT height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  // Soonest reusable first.
  std::deque<std::unique_ptr<Output>> idle;

  void Return(std::unique_ptr<Output> output) {
    D3D11_TEXTURE2D_DESC desc = {};
    output->texture->GetDesc(&desc);
    std::lock_guard<std::mutex> lock(mutex);
    if (desc.Width == width && desc.Height == height && desc.Format == format) {
      output->reusable = MonotonicMicros() + (output->shown ? kReuseDelay : 0);
      // Kept in order of |reusable|.
      auto it = idle.end();
      while (it != idle.begin() && (*(it - 1))->reusable > output->reusable) {
        --it;
      }
      idle.insert(it, std::move(output));
    }
  }
};

struct D3D11VideoProcessor::PixelPool {
  std::mutex mutex;
  // What the idle buffers hold; a buffer of another size is freed on its
  // return.
  size_t size = 0;
  std::vector<std::unique_ptr<uint8_t[]>> idle;
};

void D3D11VideoProcessor::HandleCloser::operator()(void* handle) const {
  CloseHandle(handle);
}

D3D11VideoProcessor::D3D11VideoProcessor() = default;
D3D11VideoProcessor::~D3D11VideoProcessor() = default;
D3D11VideoProcessor& D3D11VideoProcessor::operator=(
    D3D11VideoProcessor&& other) noexcept = default;

bool D3D11VideoProcessor::SetUp(ID3D11Device* device) {
  if (device == device_.Get()) {
    return video_device_ && video_context_ && completion_;
  }
  // Frames out on the old device keep its pool until they are released.
  *this = D3D11VideoProcessor();
  device_ = device;
  device_->GetImmediateContext(&context_);
  pool_ = std::make_shared<OutputPool>();
  pixels_ = std::make_shared<PixelPool>();
  D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
  context_.As(&video_context1_);
  ComPtr<ID3D11Device5> device5;
  if (SUCCEEDED(device_.As(&device5)) && SUCCEEDED(context_.As(&context4_)) &&
      SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE,
                                     IID_PPV_ARGS(&fence_)))) {
    fence_event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
  }
  if (!fence_event_) {
    fence_.Reset();
  }
  return SUCCEEDED(device_.As(&video_device_)) &&
         SUCCEEDED(context_.As(&video_context_)) &&
         SUCCEEDED(device_->CreateQuery(&query, &completion_));
}

std::unique_ptr<D3D11VideoProcessor::Output> D3D11VideoProcessor::TakeOutput(
    const D3D11_TEXTURE2D_DESC& desc) {
  std::unique_ptr<Output> reused;
  // Freed after the lock.
  std::vector<std::unique_ptr<Output>> unused;
  {
    std::lock_guard<std::mutex> lock(pool_->mutex);
    if (desc.Width != pool_->width || desc.Height != pool_->height ||
        desc.Format != pool_->format) {
      for (auto& output : pool_->idle) {
        unused.push_back(std::move(output));
      }
      pool_->idle.clear();
      pool_->width = desc.Width;
      pool_->height = desc.Height;
      pool_->format = desc.Format;
    }
    const int64_t now = MonotonicMicros();
    auto& idle = pool_->idle;
    // More than the decoder needs (left from a seek, say): freed.
    while (!idle.empty() && now - idle.front()->reusable >= kIdleLimit) {
      unused.push_back(std::move(idle.front()));
      idle.pop_front();
    }
    // The last to become reusable, so the ones in use stay few and the
    // rest go stale.
    auto ready = std::find_if(idle.begin(), idle.end(), [now](const auto& o) {
      return o->reusable > now;
    });
    while (ready != idle.begin() && !reused) {
      --ready;
      std::unique_ptr<Output> output = std::move(*ready);
      ready = idle.erase(ready);
      if (References(output->texture.Get()) == output->references) {
        reused = std::move(output);
      } else {
        // A caller kept a reference after releasing the frame (lychee.h
        // allows it): never written again.
        unused.push_back(std::move(output));
      }
    }
  }
  if (reused) {
    return reused;
  }
  auto output = std::make_unique<Output>();
  if (FAILED(device_->CreateTexture2D(&desc, nullptr, &output->texture))) {
    return nullptr;
  }
  LOGD("new %ux%u output texture", desc.Width, desc.Height);
  return output;
}

std::shared_ptr<VideoFrame> D3D11VideoProcessor::Convert(
    ID3D11Texture2D* texture,
    UINT slice,
    int width,
    int height,
    DXGI_COLOR_SPACE_TYPE color_space,
    bool hdr_output) {
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
  // HDR needs DXGI color spaces to come out of the video processor still
  // in its transfer function. DXGI has no RGB HLG, so HLG is passed off
  // as gamma 2.2 in and out: the processor then only converts YCbCr to
  // RGB and leaves the HLG signal to the shader.
  const bool pq = IsPq(color_space);
  const bool hlg = IsHlg(color_space);
  hdr_output = hdr_output && (pq || hlg);
  if ((pq || hlg) &&
      (!video_context1_ || (shader_failed_ && (!hdr_output || hlg)))) {
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC target = {};
  target.Width = output_width;
  target.Height = output_height;
  target.MipLevels = target.ArraySize = 1;
  target.Format =
      hdr_output ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
  target.SampleDesc.Count = 1;
  target.Usage = D3D11_USAGE_DEFAULT;
  target.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  target.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
  std::unique_ptr<Output> output = TakeOutput(target);
  if (!output) {
    return nullptr;
  }

  bool converted_nv12 = false;
  if (source.Format == DXGI_FORMAT_NV12 && IsSdr601Or709(color_space) &&
      !nv12_shader_failed_) {
    converted_nv12 = ConvertNv12(texture, slice, source, output.get(),
                                 output_width, output_height, color_space);
    if (!converted_nv12) {
      // Older drivers can expose NV12 only through the video processor.
      // Keep that path available when plane shader views are unsupported.
      nv12_shader_failed_ = true;
      LOGW("NV12 shader conversion unavailable; using the video processor");
    }
  }
  if (converted_nv12) {
    // The shader already wrote the output.
  } else if (pq && hdr_output) {
    // No intermediate or transfer conversion: keep the decoded HDR signal.
    if (!Blt(texture, slice, source.Format, source.Width, source.Height,
             output.get(), output_width, output_height, color_space,
             DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)) {
      return nullptr;
    }
  } else if (pq || hlg) {
    D3D11_TEXTURE2D_DESC desc = {};
    if (hdr_) {
      hdr_->texture->GetDesc(&desc);
    }
    if (!hdr_ || desc.Width != output_width || desc.Height != output_height) {
      hdr_.reset();
      hdr_view_.Reset();
      desc = target;
      // 10 bits of signal, as P010 carries them.
      desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      desc.MiscFlags = 0;
      auto hdr = std::make_unique<Output>();
      if (FAILED(device_->CreateTexture2D(&desc, nullptr, &hdr->texture)) ||
          FAILED(device_->CreateShaderResourceView(hdr->texture.Get(), nullptr,
                                                   &hdr_view_))) {
        hdr_view_.Reset();
        return nullptr;
      }
      hdr_ = std::move(hdr);
    }
    const DXGI_COLOR_SPACE_TYPE input_space =
        hlg ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020 : color_space;
    const DXGI_COLOR_SPACE_TYPE output_space =
        hlg ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020
            : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    if (!Blt(texture, slice, source.Format, source.Width, source.Height,
             hdr_.get(), output_width, output_height, input_space,
             output_space) ||
        !ConvertHdr(output.get(), output_width, output_height, hlg,
                    hdr_output)) {
      return nullptr;
    }
  } else if (!Blt(texture, slice, source.Format, source.Width, source.Height,
                  output.get(), output_width, output_height, color_space,
                  DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)) {
    return nullptr;
  }
  if (!Finish()) {
    return nullptr;
  }

  auto frame = std::make_shared<VideoFrame>();
  frame->storage = hdr_output ? VideoFrameStorage::kD3D11TextureHdr
                              : VideoFrameStorage::kD3D11Texture;
  frame->width = width;
  frame->height = height;
  frame->native_handle = output->texture.Get();
  output->references = References(output->texture.Get());
  output->shown = false;
  // The frame keeps |output| until it is released.
  frame->present = [shown = &output->shown](int64_t) { *shown = true; };
  // Back to the pool once the last copy of the frame is gone, on whatever
  // thread releases it.
  frame->owner = std::shared_ptr<void>(
      frame->native_handle, [pool = pool_, output = output.release()](void*) {
        pool->Return(std::unique_ptr<Output>(output));
      });
  return frame;
}

bool D3D11VideoProcessor::Blt(ID3D11Texture2D* texture,
                              UINT slice,
                              DXGI_FORMAT format,
                              UINT input_width,
                              UINT input_height,
                              Output* output,
                              UINT output_width,
                              UINT output_height,
                              DXGI_COLOR_SPACE_TYPE input_space,
                              DXGI_COLOR_SPACE_TYPE output_space) {
  D3D11_TEXTURE2D_DESC output_texture_desc = {};
  output->texture->GetDesc(&output_texture_desc);
  if (!processor_ || format != input_format_ ||
      output_texture_desc.Format != output_format_ ||
      input_width != input_width_ || input_height != input_height_ ||
      output_width != output_width_ || output_height != output_height_) {
    processor_.Reset();
    enumerator_.Reset();
    input_views_.clear();
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
        FAILED(
            enumerator_->CheckVideoProcessorFormat(format, &input_support)) ||
        !(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        FAILED(enumerator_->CheckVideoProcessorFormat(
            output_texture_desc.Format, &output_support)) ||
        !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ||
        FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                                   &processor_))) {
      LOGW("no video processor for format %d %ux%u", format, input_width,
           input_height);
      processor_.Reset();
      return false;
    }
    input_format_ = format;
    output_format_ = output_texture_desc.Format;
    input_width_ = input_width;
    input_height_ = input_height;
    output_width_ = output_width;
    output_height_ = output_height;
  }

  const auto key = std::make_pair(texture, slice);
  auto input_view = input_views_.find(key);
  if (input_view == input_views_.end()) {
    if (input_views_.size() >= kMaxInputViews) {
      input_views_.clear();
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc = {};
    input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    input_desc.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> view;
    if (FAILED(video_device_->CreateVideoProcessorInputView(
            texture, enumerator_.Get(), &input_desc, &view))) {
      return false;
    }
    input_view = input_views_.emplace(key, std::move(view)).first;
  }
  if (!output->view || output->enumerator != enumerator_) {
    output->view.Reset();
    output->enumerator = enumerator_;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc = {};
    output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(video_device_->CreateVideoProcessorOutputView(
            output->texture.Get(), enumerator_.Get(), &output_desc,
            &output->view))) {
      output->enumerator.Reset();
      return false;
    }
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
    video_context_->VideoProcessorSetOutputColorSpace(processor, &output_color);
  }
  video_context_->VideoProcessorSetOutputAlphaFillMode(
      processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view->second.Get();
  return SUCCEEDED(video_context_->VideoProcessorBlt(
      processor, output->view.Get(), 0, 1, &stream));
}

bool D3D11VideoProcessor::ConvertNv12(ID3D11Texture2D* texture,
                                      UINT slice,
                                      const D3D11_TEXTURE2D_DESC& source,
                                      Output* output,
                                      UINT width,
                                      UINT height,
                                      DXGI_COLOR_SPACE_TYPE color_space) {
  if (!nv12_shader_) {
    const auto vertex = CompileShader("vs", "vs_4_0");
    const auto pixel = CompileShader("ps_nv12", "ps_4_0", kNv12Shader);
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxAnisotropy = 1;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = 3 * 4 * sizeof(float);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (!vertex || !pixel ||
        FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(),
                                           vertex->GetBufferSize(), nullptr,
                                           &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(pixel->GetBufferPointer(),
                                          pixel->GetBufferSize(), nullptr,
                                          &nv12_shader_)) ||
        FAILED(device_->CreateSamplerState(&sampler, &nv12_sampler_)) ||
        FAILED(device_->CreateBuffer(&constants, nullptr, &nv12_constants_))) {
      nv12_shader_.Reset();
      return false;
    }
  }
  D3D11_TEXTURE2D_DESC desc = {};
  if (nv12_input_) nv12_input_->GetDesc(&desc);
  if (!nv12_input_ || desc.Width != source.Width ||
      desc.Height != source.Height) {
    nv12_input_.Reset();
    nv12_luma_.Reset();
    nv12_chroma_.Reset();
    desc = source;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC view = {};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    view.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &nv12_input_)) ||
        FAILED(device_->CreateShaderResourceView(nv12_input_.Get(), &view,
                                                 &nv12_luma_)))
      return false;
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(device_->CreateShaderResourceView(nv12_input_.Get(), &view,
                                                 &nv12_chroma_)))
      return false;
  }
  // Decoder arrays are commonly not shader-readable. One reusable texture
  // exposes their Y and UV planes without a GPU-to-CPU transfer.
  context_->CopySubresourceRegion(
      nv12_input_.Get(), 0, 0, 0, 0, texture,
      D3D11CalcSubresource(0, slice, source.MipLevels), nullptr);
  const bool full = color_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 ||
                    color_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
  const bool bt709 = color_space == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 ||
                     color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
  const float constants[12] = {
      bt709 ? 1.5748f : 1.402f,         bt709 ? -0.187324f : -0.344136f,
      bt709 ? -0.468124f : -0.714136f,  bt709 ? 1.8556f : 1.772f,
      full ? 0.f : 16.f / 255.f,        full ? 1.f : 255.f / 219.f,
      full ? 1.f : 255.f / 224.f,       128.f / 255.f,
      static_cast<float>(source.Width), static_cast<float>(source.Height),
      static_cast<float>(width),        static_cast<float>(height)};
  context_->UpdateSubresource(nv12_constants_.Get(), 0, nullptr, constants, 0,
                              0);
  if (!output->target && FAILED(device_->CreateRenderTargetView(
                             output->texture.Get(), nullptr, &output->target)))
    return false;
  const D3D11_VIEWPORT viewport = {
      0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  ID3D11RenderTargetView* targets[] = {output->target.Get()};
  ID3D11ShaderResourceView* views[] = {nv12_luma_.Get(), nv12_chroma_.Get()};
  ID3D11Buffer* buffers[] = {nv12_constants_.Get()};
  ID3D11SamplerState* samplers[] = {nv12_sampler_.Get()};
  context_->OMSetRenderTargets(1, targets, nullptr);
  context_->RSSetViewports(1, &viewport);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(nv12_shader_.Get(), nullptr, 0);
  context_->PSSetShaderResources(0, 2, views);
  context_->PSSetConstantBuffers(0, 1, buffers);
  context_->PSSetSamplers(0, 1, samplers);
  context_->Draw(3, 0);
  ID3D11ShaderResourceView* no_views[] = {nullptr, nullptr};
  context_->PSSetShaderResources(0, 2, no_views);
  context_->OMSetRenderTargets(0, nullptr, nullptr);
  return true;
}

bool D3D11VideoProcessor::ConvertHdr(Output* output,
                                     UINT width,
                                     UINT height,
                                     bool hlg,
                                     bool hdr_output) {
  if (!pq_shader_) {
    ComPtr<ID3DBlob> vertex = CompileShader("vs", "vs_4_0");
    ComPtr<ID3DBlob> pq = CompileShader("ps_pq", "ps_4_0");
    ComPtr<ID3DBlob> hlg_pixel = CompileShader("ps_hlg", "ps_4_0");
    ComPtr<ID3DBlob> hlg_pq = CompileShader("ps_hlg_pq", "ps_4_0");
    if (!vertex || !pq || !hlg_pixel || !hlg_pq ||
        FAILED(device_->CreateVertexShader(vertex->GetBufferPointer(),
                                           vertex->GetBufferSize(), nullptr,
                                           &vertex_shader_)) ||
        FAILED(device_->CreatePixelShader(pq->GetBufferPointer(),
                                          pq->GetBufferSize(), nullptr,
                                          &pq_shader_)) ||
        FAILED(device_->CreatePixelShader(hlg_pixel->GetBufferPointer(),
                                          hlg_pixel->GetBufferSize(), nullptr,
                                          &hlg_shader_)) ||
        FAILED(device_->CreatePixelShader(hlg_pq->GetBufferPointer(),
                                          hlg_pq->GetBufferSize(), nullptr,
                                          &hlg_pq_shader_))) {
      shader_failed_ = true;
      vertex_shader_.Reset();
      pq_shader_.Reset();
      hlg_shader_.Reset();
      hlg_pq_shader_.Reset();
      return false;
    }
  }
  if (!output->target &&
      FAILED(device_->CreateRenderTargetView(output->texture.Get(), nullptr,
                                             &output->target))) {
    return false;
  }
  // The device is the decoder's own; nothing else draws with it, so the
  // state is set without saving what was there.
  const D3D11_VIEWPORT viewport = {
      0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
  ID3D11RenderTargetView* targets[] = {output->target.Get()};
  ID3D11ShaderResourceView* views[] = {hdr_view_.Get()};
  context_->OMSetRenderTargets(1, targets, nullptr);
  context_->RSSetViewports(1, &viewport);
  context_->IASetInputLayout(nullptr);
  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
  context_->PSSetShader(hdr_output ? hlg_pq_shader_.Get()
                        : hlg      ? hlg_shader_.Get()
                                   : pq_shader_.Get(),
                        nullptr, 0);
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
  // the shared texture, but never indefinitely, and without spinning: the
  // decode thread shares the CPU with the caller's.
  const int64_t deadline = MonotonicMicros() + kCompletionTimeout;
  if (fence_) {
    const uint64_t value = ++fence_value_;
    if (SUCCEEDED(context4_->Signal(fence_.Get(), value))) {
      context_->Flush();
      // The event is reset only by a wait: one that timed out leaves it to
      // wake a later wait early, so the fence is checked after each.
      while (fence_->GetCompletedValue() < value) {
        const int64_t left = deadline - MonotonicMicros();
        if (left <= 0) {
          LOGW("video processor timed out");
          return false;
        }
        if (FAILED(fence_->SetEventOnCompletion(value, fence_event_.get()))) {
          break;
        }
        WaitForSingleObject(fence_event_.get(),
                            static_cast<DWORD>(left / kMicrosPerMilli + 1));
      }
      if (fence_->GetCompletedValue() >= value) {
        // A removed device reports every value reached.
        return SUCCEEDED(device_->GetDeviceRemovedReason());
      }
    }
  }
  context_->End(completion_.Get());
  context_->Flush();
  const int64_t began = MonotonicMicros();
  HRESULT status;
  while ((status = context_->GetData(completion_.Get(), nullptr, 0,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH)) ==
         S_FALSE) {
    const int64_t now = MonotonicMicros();
    if (now >= deadline) {
      LOGW("video processor timed out");
      return false;
    }
    if (now - began < kCompletionSpin) {
      std::this_thread::yield();
    } else {
      Sleep(1);
    }
  }
  return SUCCEEDED(status);
}

std::shared_ptr<VideoFrame> D3D11VideoProcessor::Download(
    const VideoFrame& frame) {
  auto* texture = static_cast<ID3D11Texture2D*>(frame.native_handle);
  if (frame.storage != VideoFrameStorage::kD3D11Texture || !texture ||
      !context_) {
    return nullptr;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  if (staging_) {
    staging_->GetDesc(&desc);
  }
  if (!staging_ || desc.Width != static_cast<UINT>(frame.width) ||
      desc.Height != static_cast<UINT>(frame.height)) {
    staging_.Reset();
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
      return nullptr;
    }
  }
  context_->CopyResource(staging_.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return nullptr;
  }
  const int stride = frame.width * 4;
  const size_t size = static_cast<size_t>(stride) * frame.height;
  // A frame's worth is large enough that a fresh allocation comes from the
  // system and faults in every page on the copy.
  std::unique_ptr<uint8_t[]> pixels;
  // Freed after the lock.
  std::vector<std::unique_ptr<uint8_t[]>> unused;
  {
    std::lock_guard<std::mutex> lock(pixels_->mutex);
    if (pixels_->size != size) {
      unused.swap(pixels_->idle);
      pixels_->size = size;
    } else if (!pixels_->idle.empty()) {
      pixels = std::move(pixels_->idle.back());
      pixels_->idle.pop_back();
    }
  }
  if (!pixels) {
    pixels = std::make_unique_for_overwrite<uint8_t[]>(size);
  }
  for (int y = 0; y < frame.height; ++y) {
    std::memcpy(pixels.get() + static_cast<size_t>(y) * stride,
                static_cast<const uint8_t*>(mapped.pData) +
                    static_cast<size_t>(y) * mapped.RowPitch,
                stride);
  }
  context_->Unmap(staging_.Get(), 0);
  auto result = std::make_shared<VideoFrame>();
  result->storage = VideoFrameStorage::kCpuBgra;
  result->width = frame.width;
  result->height = frame.height;
  result->pts = frame.pts;
  result->duration = frame.duration;
  result->data = pixels.get();
  result->stride = stride;
  result->owner = std::shared_ptr<void>(
      pixels.release(), [pool = pixels_, size](void* data) {
        std::unique_ptr<uint8_t[]> pixels(static_cast<uint8_t*>(data));
        std::lock_guard<std::mutex> lock(pool->mutex);
        if (pool->size == size && pool->idle.size() < kMaxIdlePixels) {
          pool->idle.push_back(std::move(pixels));
        }
      });
  return result;
}

}  // namespace lychee
