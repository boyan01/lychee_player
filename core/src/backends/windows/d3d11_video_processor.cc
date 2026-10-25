#define LOG_TAG "d3d11"

#include "backends/windows/d3d11_video_processor.h"

#include <thread>

#include "base/log.h"
#include "base/time.h"

namespace lychee {

namespace {

using Microsoft::WRL::ComPtr;

// Longest wait for the GPU to finish a conversion.
constexpr int64_t kCompletionTimeout = 100 * kMicrosPerMilli;

}  // namespace

bool D3D11VideoProcessor::SetUp(ID3D11Device* device) {
  if (device == device_.Get()) {
    return video_device_ && video_context_ && completion_;
  }
  *this = D3D11VideoProcessor();
  device_ = device;
  device_->GetImmediateContext(&context_);
  D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
  return SUCCEEDED(device_.As(&video_device_)) &&
         SUCCEEDED(context_.As(&video_context_)) &&
         SUCCEEDED(device_->CreateQuery(&query, &completion_));
}

std::shared_ptr<VideoFrame> D3D11VideoProcessor::Convert(
    ID3D11Texture2D* texture,
    UINT slice,
    int width,
    int height,
    bool bt709,
    bool full_range) {
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

  if (!processor_ || source.Format != input_format_ ||
      source.Width != input_width_ || source.Height != input_height_ ||
      output_width != output_width_ || output_height != output_height_) {
    processor_.Reset();
    enumerator_.Reset();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = content.OutputFrameRate = {60, 1};
    content.InputWidth = source.Width;
    content.InputHeight = source.Height;
    content.OutputWidth = output_width;
    content.OutputHeight = output_height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    UINT input_support = 0;
    UINT output_support = 0;
    if (FAILED(video_device_->CreateVideoProcessorEnumerator(&content,
                                                             &enumerator_)) ||
        FAILED(enumerator_->CheckVideoProcessorFormat(source.Format,
                                                      &input_support)) ||
        !(input_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        FAILED(enumerator_->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &output_support)) ||
        !(output_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ||
        FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0,
                                                   &processor_))) {
      LOGW("no video processor for format %d %ux%u", source.Format,
           source.Width, source.Height);
      processor_.Reset();
      return nullptr;
    }
    input_format_ = source.Format;
    input_width_ = source.Width;
    input_height_ = source.Height;
    output_width_ = output_width;
    output_height_ = output_height;
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
          output.Get(), enumerator_.Get(), &output_desc, &output_view))) {
    return nullptr;
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
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_color = {};
  input_color.YCbCr_Matrix = bt709 ? 1 : 0;
  input_color.Nominal_Range = full_range
                                  ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                  : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_color = {};
  video_context_->VideoProcessorSetStreamColorSpace(processor, 0,
                                                    &input_color);
  video_context_->VideoProcessorSetOutputColorSpace(processor, &output_color);
  video_context_->VideoProcessorSetOutputAlphaFillMode(
      processor, D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
  D3D11_VIDEO_PROCESSOR_STREAM stream = {};
  stream.Enable = TRUE;
  stream.pInputSurface = input_view.Get();
  if (FAILED(video_context_->VideoProcessorBlt(processor, output_view.Get(), 0,
                                               1, &stream))) {
    return nullptr;
  }

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
      return nullptr;
    }
    std::this_thread::yield();
  }
  if (FAILED(status)) {
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

}  // namespace lychee
