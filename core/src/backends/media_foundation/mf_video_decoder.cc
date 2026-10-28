#define LOG_TAG "mf_video"

#include "backends/media_foundation/mf_video_decoder.h"

#include <codecapi.h>
#include <d3d10.h>
#include <wmcodecdsp.h>

#include <algorithm>

#include "base/log.h"

namespace lychee {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMaxConsecutiveErrors = 10;

ComPtr<IMFTransform> CreateDecoderMft(const GUID& subtype) {
  ComPtr<IMFTransform> decoder;
  if (subtype == MFVideoFormat_H264) {
    CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&decoder));
    return decoder;
  }
  // HEVC, VP9 and AV1 come with their Store extensions, if installed.
  MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, subtype};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
                       MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                           MFT_ENUM_FLAG_SORTANDFILTER,
                       &input, nullptr, &activates, &count))) {
    return nullptr;
  }
  for (UINT32 i = 0; i < count; ++i) {
    if (!decoder) {
      activates[i]->ActivateObject(IID_PPV_ARGS(&decoder));
    }
    activates[i]->Release();
  }
  CoTaskMemFree(activates);
  return decoder;
}

// The H.273 code points of what the decoder read from the stream, over
// what the container said.
ColorInfo OutputColor(IMFMediaType* type, ColorInfo color) {
  UINT32 value = 0;
  if (SUCCEEDED(type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &value))) {
    switch (value) {
      case MFVideoTransFunc_2084:
        color.transfer = 16;
        break;
      case MFVideoTransFunc_HLG:
        color.transfer = 18;
        break;
      case MFVideoTransFunc_709:
        color.transfer = 1;
        break;
      default:
        break;
    }
  }
  if (SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &value))) {
    switch (value) {
      case MFVideoTransferMatrix_BT709:
        color.matrix = 1;
        break;
      case MFVideoTransferMatrix_BT601:
        color.matrix = 6;
        break;
      case MFVideoTransferMatrix_BT2020_10:
      case MFVideoTransferMatrix_BT2020_12:
        color.matrix = 9;
        break;
      default:
        break;
    }
  }
  if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &value)) &&
      value != MFNominalRange_Unknown) {
    color.full_range = value == MFNominalRange_0_255;
  }
  return color;
}

}  // namespace

MFVideoDecoder::MFVideoDecoder(uint32_t accepted_storage,
                               VideoDevice device,
                               bool hardware)
    : device_(std::move(device)),
      hardware_(hardware),
      output_textures_(
          device_.type == VideoDevice::Type::kD3D11Adapter &&
          (accepted_storage &
           (static_cast<uint32_t>(VideoFrameStorage::kD3D11Texture) |
            static_cast<uint32_t>(VideoFrameStorage::kD3D11TextureHdr)))),
      output_hdr_(
          output_textures_ &&
          (accepted_storage &
           static_cast<uint32_t>(VideoFrameStorage::kD3D11TextureHdr))) {}

MFVideoDecoder::~MFVideoDecoder() {
  if (decoder_) {
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  }
}

bool MFVideoDecoder::Initialize(const VideoConfig& config, std::string* error) {
  GUID subtype = GUID_NULL;
  switch (config.codec) {
    case VideoCodec::kH264:
      subtype = MFVideoFormat_H264;
      break;
    case VideoCodec::kHEVC:
      subtype = MFVideoFormat_HEVC;
      break;
    case VideoCodec::kVP9:
      subtype = MFVideoFormat_VP90;
      break;
    case VideoCodec::kAV1:
      subtype = MFVideoFormat_AV1;
      break;
    default:
      *error = "mediafoundation: unsupported video codec";
      return false;
  }
  const bool nal_units =
      config.codec == VideoCodec::kH264 || config.codec == VideoCodec::kHEVC;
  if (nal_units && !converter_.Initialize(config)) {
    *error = "mediafoundation: invalid codec configuration";
    return false;
  }
  config_ = config;
  if (!StartMediaFoundation()) {
    *error = "mediafoundation: cannot start Media Foundation";
    return false;
  }
  decoder_ = CreateDecoderMft(subtype);
  if (!decoder_) {
    const char* extension =
        config.codec == VideoCodec::kHEVC  ? "HEVC Video Extensions"
        : config.codec == VideoCodec::kVP9 ? "VP9 Video Extensions"
        : config.codec == VideoCodec::kAV1 ? "AV1 Video Extension"
                                           : "Media Feature Pack";
    *error = std::string("mediafoundation: no ") +
             VideoCodecName(config.codec) + " decoder (" + extension + ")";
    return false;
  }
  ComPtr<IMFAttributes> attributes;
  if (FAILED(decoder_->GetAttributes(&attributes)) ||
      (hardware_ &&
       !MFGetAttributeUINT32(attributes.Get(), MF_SA_D3D11_AWARE, FALSE))) {
    *error = "mediafoundation: the decoder cannot decode on D3D11";
    return false;
  }
  if (config.low_delay) {
    attributes->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
  }
  if (!CreateDevice(error)) {
    return false;
  }
  HRESULT hr = S_OK;
  // Without a device manager the decoder decodes on the CPU.
  if (hardware_ &&
      FAILED(hr = decoder_->ProcessMessage(
                 MFT_MESSAGE_SET_D3D_MANAGER,
                 reinterpret_cast<ULONG_PTR>(device_manager_.Get())))) {
    *error =
        "mediafoundation: MFT_MESSAGE_SET_D3D_MANAGER: " + HResultString(hr);
    return false;
  }

  ComPtr<IMFMediaType> input;
  if (FAILED(MFCreateMediaType(&input))) {
    *error = "mediafoundation: MFCreateMediaType failed";
    return false;
  }
  input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input->SetGUID(MF_MT_SUBTYPE, subtype);
  // VPx uses progressive input (as in Chromium's
  // media/gpu/windows/dxva_video_decode_accelerator_win.cc). AV1 is
  // progressive too; H.264 and HEVC may interlace.
  input->SetUINT32(MF_MT_INTERLACE_MODE,
                   nal_units ? MFVideoInterlace_MixedInterlaceOrProgressive
                             : MFVideoInterlace_Progressive);
  if (config.low_delay && config.codec == VideoCodec::kHEVC && !hardware_) {
    // A live packet is one complete picture. Without explicit NAL
    // lengths the software HEVC MFT waits for the next sample to find
    // the picture's end, retaining the last frame indefinitely. A drain
    // releases it but discards the reference pictures needed on resume.
    // MF_NALU_LENGTH_SET / MF_NALU_LENGTH_INFORMATION let it finish each
    // picture while keeping those references (Microsoft's HEVC decoder
    // and NALU length attribute documentation).
    input->SetUINT32(MF_NALU_LENGTH_SET, TRUE);
  }
  if (config.width > 0 && config.height > 0) {
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, config.width,
                       config.height);
  }
  hr = decoder_->SetInputType(0, input.Get(), 0);
  if (FAILED(hr)) {
    *error = "mediafoundation: the decoder rejects this stream: " +
             HResultString(hr);
    return false;
  }
  // Without the stream's parameter sets the decoder may offer no output
  // type yet; it asks for one with MF_E_TRANSFORM_STREAM_CHANGE.
  SetOutputType();
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  LOGI("%s decoder opened for %s %dx%d%s", name(), VideoCodecName(config.codec),
       config.width, config.height, hardware_ ? "" : " (software)");
  return true;
}

bool MFVideoDecoder::CreateDevice(std::string* error) {
  auto* adapter = device_.type == VideoDevice::Type::kD3D11Adapter
                      ? static_cast<IDXGIAdapter*>(device_.handle.get())
                      : nullptr;
  HRESULT hr = D3D11CreateDevice(
      adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
      &d3d11_device_, nullptr, nullptr);
  if (FAILED(hr) && !hardware_ && !adapter) {
    // No GPU: software decoding converts frames on WARP.
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &d3d11_device_, nullptr, nullptr);
  }
  if (FAILED(hr)) {
    *error = "mediafoundation: D3D11CreateDevice: " + HResultString(hr);
    return false;
  }
  // The decoder uses the device from its own threads too.
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(d3d11_device_.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }
  UINT token = 0;
  if (FAILED(hr = MFCreateDXGIDeviceManager(&token, &device_manager_)) ||
      FAILED(hr = device_manager_->ResetDevice(d3d11_device_.Get(), token))) {
    *error = "mediafoundation: no DXGI device manager: " + HResultString(hr);
    return false;
  }
  return true;
}

bool MFVideoDecoder::SetOutputType() {
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> type;
    if (FAILED(decoder_->GetOutputAvailableType(0, i, &type))) {
      return false;
    }
    GUID subtype = {};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        (subtype != MFVideoFormat_NV12 && subtype != MFVideoFormat_P010) ||
        FAILED(decoder_->SetOutputType(0, type.Get(), 0))) {
      continue;
    }
    UINT32 width = 0;
    UINT32 height = 0;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    format_ =
        subtype == MFVideoFormat_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    coded_width_ = static_cast<int>(width);
    coded_height_ = static_cast<int>(height);
    // Coded sizes are whole macroblocks (1088 lines for 1080p); the
    // aperture is the picture.
    MFVideoArea area = {};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                reinterpret_cast<UINT8*>(&area), sizeof(area),
                                nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0 &&
        static_cast<UINT32>(area.Area.cx) <= width &&
        static_cast<UINT32>(area.Area.cy) <= height) {
      width = static_cast<UINT32>(area.Area.cx);
      height = static_cast<UINT32>(area.Area.cy);
    }
    width_ = static_cast<int>(width);
    height_ = static_cast<int>(height);
    const ColorInfo color = OutputColor(type.Get(), config_.color);
    color_space_ = DxgiColorSpace(color, height_);
    MFT_OUTPUT_STREAM_INFO info = {};
    provides_samples_ = SUCCEEDED(decoder_->GetOutputStreamInfo(0, &info)) &&
                        (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES);
    LOGI("%s output %s %dx%d transfer=%d matrix=%d%s", name(),
         subtype == MFVideoFormat_P010 ? "P010" : "NV12", width_, height_,
         color.transfer, color.matrix, provides_samples_ ? "" : " (memory)");
    return width_ > 0 && height_ > 0;
  }
}

bool MFVideoDecoder::Decode(const EncodedPacket& packet,
                            const OutputCallback& output) {
  if (packet.end_of_stream) {
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    const bool ok = Drain(output);
    decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return ok && !DeviceLost();
  }
  if (!packet.buffer) {
    return true;
  }
  const uint8_t* data = packet.buffer->data();
  size_t size = packet.size();
  if (config_.codec == VideoCodec::kH264 ||
      config_.codec == VideoCodec::kHEVC) {
    // Annex B, with parameter sets again at every keyframe: a flush
    // forgets them. VP9 frames and AV1 OBUs already have their wire format.
    annexb_.clear();
    if (!converter_.Convert(data, size, packet.keyframe, &annexb_)) {
      LOGW("%s: malformed sample", name());
      return ++consecutive_errors_ < kMaxConsecutiveErrors;
    }
    data = annexb_.data();
    size = annexb_.size();
  }
  ComPtr<IMFSample> sample =
      CreateMFSample(data, size, packet.pts, packet.duration);
  if (!sample) {
    LOGE("%s: cannot make a sample", name());
    return false;
  }
  if (config_.low_delay && config_.codec == VideoCodec::kHEVC && !hardware_) {
    // Each length includes its Annex B start code, so the spans cover
    // the entire input sample, including in-band parameter sets.
    std::vector<DWORD> lengths;
    size_t previous = 0;
    for (size_t i = 0; i + 3 <= annexb_.size(); ++i) {
      size_t prefix = 0;
      if (annexb_[i] == 0 && annexb_[i + 1] == 0) {
        if (annexb_[i + 2] == 1) {
          prefix = 3;
        } else if (i + 4 <= annexb_.size() && annexb_[i + 2] == 0 &&
                   annexb_[i + 3] == 1) {
          prefix = 4;
        }
      }
      if (prefix != 0) {
        if (i > previous) {
          lengths.push_back(static_cast<DWORD>(i - previous));
        }
        previous = i;
        i += prefix - 1;
      }
    }
    lengths.push_back(static_cast<DWORD>(annexb_.size() - previous));
    if (lengths.size() > MAXDWORD / sizeof(DWORD) ||
        FAILED(sample->SetBlob(
            MF_NALU_LENGTH_INFORMATION,
            reinterpret_cast<const uint8_t*>(lengths.data()),
            static_cast<UINT32>(lengths.size() * sizeof(DWORD))))) {
      LOGE("%s: cannot describe the input NAL lengths", name());
      return false;
    }
  }
  HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
  if (hr == MF_E_NOTACCEPTING) {
    if (!Drain(output)) {
      return false;
    }
    hr = decoder_->ProcessInput(0, sample.Get(), 0);
  }
  if (FAILED(hr)) {
    LOGW("%s: ProcessInput: %s", name(), HResultString(hr).c_str());
    consecutive_errors_++;
  }
  if (!Drain(output) || DeviceLost()) {
    return false;
  }
  return consecutive_errors_ < kMaxConsecutiveErrors;
}

bool MFVideoDecoder::Drain(const OutputCallback& output) {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER buffer = {};
    ComPtr<IMFSample> sample;
    if (!hardware_ && !provides_samples_) {
      // A software decoder writes into a sample of ours.
      MFT_OUTPUT_STREAM_INFO info = {};
      decoder_->GetOutputStreamInfo(0, &info);
      sample = CreateMFOutputSample(std::max<DWORD>(info.cbSize, 1));
      if (!sample) {
        return false;
      }
      buffer.pSample = sample.Get();
    }
    DWORD status = 0;
    HRESULT hr = decoder_->ProcessOutput(0, 1, &buffer, &status);
    if (buffer.pSample != sample.Get()) {
      sample.Attach(buffer.pSample);
    }
    if (buffer.pEvents) {
      buffer.pEvents->Release();
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return true;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (!SetOutputType()) {
        LOGW("%s: no NV12 or P010 output for this stream", name());
        return false;
      }
      continue;
    }
    if (FAILED(hr)) {
      LOGW("%s: ProcessOutput: %s", name(), HResultString(hr).c_str());
      return false;
    }
    if (hardware_ && !provides_samples_) {
      // Decoding to system memory: the GPU is not decoding.
      LOGW("%s: the decoder does not decode on the GPU", name());
      return false;
    }
    if (!sample) {
      continue;
    }
    consecutive_errors_ = 0;
    std::shared_ptr<VideoFrame> frame = CreateFrame(sample.Get());
    // Released before the frame goes out, so the decoder's pool never
    // waits on frames the renderer queues.
    sample.Reset();
    if (!frame) {
      return false;
    }
    output(std::move(frame));
  }
}

bool MFVideoDecoder::Upload(IMFSample* sample) {
  if (coded_width_ <= 0 || coded_height_ <= 0) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  if (upload_) {
    upload_->GetDesc(&desc);
  }
  if (!upload_ || desc.Format != format_ ||
      desc.Width != static_cast<UINT>(coded_width_) ||
      desc.Height != static_cast<UINT>(coded_height_)) {
    upload_.Reset();
    desc = {};
    desc.Width = static_cast<UINT>(coded_width_);
    desc.Height = static_cast<UINT>(coded_height_);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format_;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(d3d11_device_->CreateTexture2D(&desc, nullptr, &upload_))) {
      LOGW("%s: cannot make a %dx%d upload texture", name(), coded_width_,
           coded_height_);
      return false;
    }
  }
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
    return false;
  }
  // Both planes, the chroma right after the coded height of luma rows,
  // which is how UpdateSubresource() takes planar formats.
  const LONG row_bytes = coded_width_ * (format_ == DXGI_FORMAT_P010 ? 2 : 1);
  ComPtr<IMF2DBuffer> buffer_2d;
  BYTE* data = nullptr;
  LONG pitch = 0;
  DWORD length = 0;
  bool ok = false;
  if (SUCCEEDED(buffer.As(&buffer_2d)) &&
      SUCCEEDED(buffer_2d->Lock2D(&data, &pitch))) {
    ok = pitch >= row_bytes;
    if (ok) {
      ComPtr<ID3D11DeviceContext> context;
      d3d11_device_->GetImmediateContext(&context);
      context->UpdateSubresource(upload_.Get(), 0, nullptr, data,
                                 static_cast<UINT>(pitch), 0);
    }
    buffer_2d->Unlock2D();
  } else if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
    ok = length >= static_cast<DWORD>(row_bytes) * coded_height_ * 3 / 2;
    if (ok) {
      ComPtr<ID3D11DeviceContext> context;
      d3d11_device_->GetImmediateContext(&context);
      context->UpdateSubresource(upload_.Get(), 0, nullptr, data,
                                 static_cast<UINT>(row_bytes), 0);
    }
    buffer->Unlock();
  }
  return ok;
}

std::shared_ptr<VideoFrame> MFVideoDecoder::CreateFrame(IMFSample* sample) {
  ComPtr<ID3D11Texture2D> texture;
  UINT slice = 0;
  if (!hardware_) {
    if (!Upload(sample)) {
      LOGW("%s: cannot upload a frame", name());
      return nullptr;
    }
    texture = upload_;
  } else {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) ||
        FAILED(buffer.As(&dxgi)) ||
        FAILED(dxgi->GetResource(IID_PPV_ARGS(&texture))) ||
        FAILED(dxgi->GetSubresourceIndex(&slice))) {
      LOGW("%s: output is not a D3D11 texture", name());
      return nullptr;
    }
  }
  std::shared_ptr<VideoFrame> frame = processor_.Convert(
      texture.Get(), slice, width_, height_, color_space_, output_hdr_);
  if (frame && !output_textures_) {
    frame = processor_.Download(*frame);
  }
  if (!frame) {
    LOGW("%s: cannot convert a frame", name());
    return nullptr;
  }
  if (!logged_output_) {
    logged_output_ = true;
    LOGI("%s frames go out as %s", name(),
         output_textures_ ? "D3D11 textures" : "CPU BGRA");
  }
  LONGLONG time = 0;
  frame->pts =
      SUCCEEDED(sample->GetSampleTime(&time)) ? time / 10 : kNoTimestamp;
  frame->duration =
      SUCCEEDED(sample->GetSampleDuration(&time)) && time > 0 ? time / 10 : 0;
  return frame;
}

bool MFVideoDecoder::DeviceLost() {
  // After a GPU reset every call fails, or frames silently fail to convert;
  // report it at once so the renderer can rebuild on a new device.
  HRESULT reason = d3d11_device_->GetDeviceRemovedReason();
  if (FAILED(reason)) {
    LOGW("D3D11 device removed: %s", HResultString(reason).c_str());
    return true;
  }
  return false;
}

void MFVideoDecoder::Flush() {
  decoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  consecutive_errors_ = 0;
}

}  // namespace lychee
