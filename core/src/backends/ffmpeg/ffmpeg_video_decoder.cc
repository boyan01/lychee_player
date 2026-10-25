#define LOG_TAG "ffmpeg_video"

#include "backends/ffmpeg/ffmpeg_video_decoder.h"

#include "base/log.h"

#if defined(__APPLE__)
#include "backends/apple/pixel_buffer.h"
#elif defined(_WIN32)
#include <d3d11_4.h>
#include <wrl/client.h>

#include "backends/windows/d3d11_video_processor.h"
#elif defined(__linux__) && !defined(__ANDROID__)
#define LYCHEE_VAAPI 1
#include <string>

#include "lychee/lychee.h"
#endif

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#elif defined(LYCHEE_VAAPI)
#include <libavutil/hwcontext_drm.h>
#endif
}

namespace lychee {

namespace {

AVHWDeviceType PlatformHardwareDeviceType() {
#if defined(__APPLE__)
  return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#elif defined(_WIN32)
  return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(LYCHEE_VAAPI)
  return AV_HWDEVICE_TYPE_VAAPI;
#else
  return AV_HWDEVICE_TYPE_NONE;
#endif
}

#if defined(_WIN32)
// FFmpeg's D3D11VA device on |adapter|: av_hwdevice_ctx_create() only takes
// an adapter index.
int CreateD3D11VADevice(IDXGIAdapter* adapter, AVBufferRef** result) {
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                               D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, nullptr))) {
    return AVERROR_EXTERNAL;
  }
  // As av_hwdevice_ctx_create() does for the devices it creates.
  Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
  if (SUCCEEDED(device.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }
  AVBufferRef* context = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!context) {
    return AVERROR(ENOMEM);
  }
  auto* hw = reinterpret_cast<AVHWDeviceContext*>(context->data);
  // FFmpeg releases it with the context.
  static_cast<AVD3D11VADeviceContext*>(hw->hwctx)->device = device.Detach();
  int ret = av_hwdevice_ctx_init(context);
  if (ret < 0) {
    av_buffer_unref(&context);
    return ret;
  }
  *result = context;
  return 0;
}

bool IsBt709(const AVFrame* frame) {
  return frame->colorspace == AVCOL_SPC_UNSPECIFIED
             ? frame->height > 576
             : frame->colorspace != AVCOL_SPC_BT470BG &&
                   frame->colorspace != AVCOL_SPC_SMPTE170M;
}
#elif defined(LYCHEE_VAAPI)
// Frames that the decoder's surfaces are held by outside it: the renderer's
// queue, the frame on screen and the one the caller still draws from.
// VAAPI surface pools do not grow.
constexpr int kHeldDmabufFrames = 8;

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
  return static_cast<uint32_t>(a) | static_cast<uint32_t>(b) << 8 |
         static_cast<uint32_t>(c) << 16 | static_cast<uint32_t>(d) << 24;
}
// drm_fourcc.h, which is not always installed.
constexpr uint32_t kDrmFormatR8 = Fourcc('R', '8', ' ', ' ');
constexpr uint32_t kDrmFormatR16 = Fourcc('R', '1', '6', ' ');
constexpr uint32_t kDrmFormatNv12 = Fourcc('N', 'V', '1', '2');
constexpr uint32_t kDrmFormatP010 = Fourcc('P', '0', '1', '0');

struct DmabufFrame {
  // Keeps the file descriptors open and the decoder's surface referenced.
  ScopedAVFrame mapped;
  lychee_dmabuf dmabuf = {};
};

// |frame|, a VAAPI surface, mapped to DRM PRIME buffers; null for layouts
// other than NV12 and P010.
std::shared_ptr<VideoFrame> WrapDmabuf(const AVFrame* frame) {
  auto owner = std::make_shared<DmabufFrame>();
  owner->mapped.reset(av_frame_alloc());
  if (!owner->mapped) {
    return nullptr;
  }
  owner->mapped->format = AV_PIX_FMT_DRM_PRIME;
  if (av_hwframe_map(owner->mapped.get(), frame, AV_HWFRAME_MAP_READ) < 0) {
    return nullptr;
  }
  const auto* descriptor =
      reinterpret_cast<const AVDRMFrameDescriptor*>(owner->mapped->data[0]);
  lychee_dmabuf& dmabuf = owner->dmabuf;
  dmabuf.struct_size = sizeof(dmabuf);
  // FFmpeg exports VAAPI surfaces a layer per plane (R8 and GR88, or R16
  // and GR1616); other exporters give one layer with both planes.
  if (descriptor->nb_layers == 1) {
    dmabuf.fourcc = descriptor->layers[0].format;
  } else if (descriptor->nb_layers == 2) {
    dmabuf.fourcc =
        descriptor->layers[0].format == kDrmFormatR8    ? kDrmFormatNv12
        : descriptor->layers[0].format == kDrmFormatR16 ? kDrmFormatP010
                                                        : 0;
  }
  if (dmabuf.fourcc != kDrmFormatNv12 && dmabuf.fourcc != kDrmFormatP010) {
    return nullptr;
  }
  for (int i = 0; i < descriptor->nb_layers; ++i) {
    const AVDRMLayerDescriptor& layer = descriptor->layers[i];
    for (int j = 0; j < layer.nb_planes && dmabuf.plane_count < 4; ++j) {
      const AVDRMPlaneDescriptor& plane = layer.planes[j];
      dmabuf.fd[dmabuf.plane_count] =
          descriptor->objects[plane.object_index].fd;
      dmabuf.offset[dmabuf.plane_count] = static_cast<uint32_t>(plane.offset);
      dmabuf.pitch[dmabuf.plane_count] = static_cast<uint32_t>(plane.pitch);
      dmabuf.plane_count++;
    }
  }
  if (dmabuf.plane_count != 2) {
    return nullptr;
  }
  dmabuf.modifier = descriptor->objects[0].format_modifier;
  dmabuf.width = frame->width;
  dmabuf.height = frame->height;
  // AVColorSpace values are the H.273 matrix coefficients.
  dmabuf.color_matrix =
      frame->colorspace != AVCOL_SPC_UNSPECIFIED
          ? frame->colorspace
          : (frame->height > 576 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M);
  dmabuf.full_range = frame->color_range == AVCOL_RANGE_JPEG;

  auto result = std::make_shared<VideoFrame>();
  result->storage = VideoFrameStorage::kDmabuf;
  result->width = frame->width;
  result->height = frame->height;
  result->native_handle = &owner->dmabuf;
  result->owner = std::move(owner);
  return result;
}
#endif

struct CpuFrameData {
  uint8_t* data = nullptr;
  ~CpuFrameData() { av_free(data); }
};

}  // namespace

FFmpegVideoDecoder::FFmpegVideoDecoder(bool hardware,
                                       uint32_t accepted_storage,
                                       std::shared_ptr<void> output)
    : hardware_(hardware),
      accepted_storage_(accepted_storage),
      // swscale writes either order at the same cost; choosing the caller's
      // saves a second pass over every frame.
      cpu_storage_((accepted_storage &
                    static_cast<uint32_t>(VideoFrameStorage::kCpuRgba)) &&
                           !(accepted_storage &
                             static_cast<uint32_t>(VideoFrameStorage::kCpuBgra))
                       ? VideoFrameStorage::kCpuRgba
                       : VideoFrameStorage::kCpuBgra),
      output_(std::move(output)),
      hardware_output_(hardware),
      frame_(av_frame_alloc()),
      av_packet_(av_packet_alloc()) {}

FFmpegVideoDecoder::~FFmpegVideoDecoder() {
  context_.reset();
  av_buffer_unref(&hw_device_context_);
  sws_free_context(&sws_context_);
}

const char* FFmpegVideoDecoder::name() const {
  return hardware_ ? "ffmpeg-hwaccel" : "ffmpeg";
}

bool FFmpegVideoDecoder::Initialize(const VideoConfig& config,
                                    std::string* error) {
  AVCodecParameters* parameters = CreateCodecParameters(config);
  const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
  if (!codec) {
    avcodec_parameters_free(&parameters);
    *error = "no decoder for " + config.codec_name;
    return false;
  }
  context_.reset(avcodec_alloc_context3(codec));
  int ret = avcodec_parameters_to_context(context_.get(), parameters);
  avcodec_parameters_free(&parameters);
  if (ret < 0) {
    *error = "avcodec_parameters_to_context: " + AVErrorString(ret);
    return false;
  }
  context_->pkt_timebase = kMicrosTimeBase;

  if (hardware_) {
    if (!InitializeHardware(codec, error)) {
      return false;
    }
    // Hardware decoders do their own pipelining.
    context_->thread_count = 1;
  } else {
    context_->thread_count = 0;
    context_->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
  }

  ret = avcodec_open2(context_.get(), codec, nullptr);
  if (ret < 0) {
    *error = "avcodec_open2: " + AVErrorString(ret);
    return false;
  }
  LOGI("%s decoder opened for %s %dx%d", name(), codec->name, config.width,
       config.height);
  return true;
}

bool FFmpegVideoDecoder::InitializeHardware(const AVCodec* codec,
                                            std::string* error) {
  hw_device_type_ = PlatformHardwareDeviceType();
  if (hw_device_type_ == AV_HWDEVICE_TYPE_NONE) {
    *error = "no hardware decoding on this platform";
    return false;
  }
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* hw_config = avcodec_get_hw_config(codec, i);
    if (!hw_config) {
      break;
    }
    if (hw_config->device_type == hw_device_type_ &&
        (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
      hw_pixel_format_ = hw_config->pix_fmt;
      break;
    }
  }
  if (hw_pixel_format_ == AV_PIX_FMT_NONE) {
    *error = std::string(codec->name) + " has no " +
             av_hwdevice_get_type_name(hw_device_type_) + " support";
    return false;
  }
  int ret;
#if defined(_WIN32)
  if (output_) {
    ret = CreateD3D11VADevice(static_cast<IDXGIAdapter*>(output_.get()),
                              &hw_device_context_);
  } else
#elif defined(LYCHEE_VAAPI)
  if (output_) {
    const std::string& node = *static_cast<const std::string*>(output_.get());
    ret = av_hwdevice_ctx_create(&hw_device_context_, hw_device_type_,
                                 node.empty() ? nullptr : node.c_str(), nullptr,
                                 0);
    context_->extra_hw_frames = kHeldDmabufFrames;
  } else
#endif
  {
    ret = av_hwdevice_ctx_create(&hw_device_context_, hw_device_type_, nullptr,
                                 nullptr, 0);
  }
  if (ret < 0) {
    *error = "av_hwdevice_ctx_create: " + AVErrorString(ret);
    return false;
  }
  context_->hw_device_ctx = av_buffer_ref(hw_device_context_);
  context_->opaque = this;
  context_->get_format = &GetFormat;
  return true;
}

// static
AVPixelFormat FFmpegVideoDecoder::GetFormat(AVCodecContext* context,
                                            const AVPixelFormat* formats) {
  auto* self = static_cast<FFmpegVideoDecoder*>(context->opaque);
  for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE;
       ++format) {
    if (*format != self->hw_pixel_format_) {
      continue;
    }
    return *format;
  }
  // FFmpeg calls us again without the hardware format if hwaccel
  // initialization fails; fall back to software output.
  LOGW("hardware format unavailable, decoding in software");
  return avcodec_default_get_format(context, formats);
}

bool FFmpegVideoDecoder::Decode(const EncodedPacket& packet,
                                const OutputCallback& output) {
  int ret;
  if (packet.end_of_stream) {
    ret = avcodec_send_packet(context_.get(), nullptr);
  } else {
    ret = WrapInAVPacket(packet, av_packet_.get());
    if (ret >= 0) {
      ret = avcodec_send_packet(context_.get(), av_packet_.get());
      av_packet_unref(av_packet_.get());
    }
  }
  if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    // Corrupt data: skip the packet and keep going.
    LOGW("avcodec_send_packet: %s", AVErrorString(ret).c_str());
    consecutive_errors_++;
  }
  ReceiveFrames(output);
  return consecutive_errors_ < kMaxConsecutiveDecodeErrors;
}

void FFmpegVideoDecoder::ReceiveFrames(const OutputCallback& output) {
  for (;;) {
    int ret = avcodec_receive_frame(context_.get(), frame_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return;
    }
    if (ret < 0) {
      LOGW("avcodec_receive_frame: %s", AVErrorString(ret).c_str());
      consecutive_errors_++;
      return;
    }
    consecutive_errors_ = 0;
    hardware_output_ = frame_->format == hw_pixel_format_;
    std::shared_ptr<VideoFrame> frame = CreateFrame(frame_.get());
    av_frame_unref(frame_.get());
    if (frame) {
      output(std::move(frame));
    }
  }
}

std::shared_ptr<VideoFrame> FFmpegVideoDecoder::CreateFrame(AVFrame* frame) {
  if (!logged_output_format_) {
    logged_output_format_ = true;
    LOGI("%s output format: %s", name(),
         av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
  }
  std::shared_ptr<VideoFrame> result;
#if defined(__APPLE__)
  if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
    result = WrapPixelBuffer(frame->data[3]);
  }
#elif defined(_WIN32)
  if (frame->format == AV_PIX_FMT_D3D11 && output_ &&
      (accepted_storage_ &
       static_cast<uint32_t>(VideoFrameStorage::kD3D11Texture))) {
    if (!d3d11_processor_) {
      d3d11_processor_ = std::make_unique<D3D11VideoProcessor>();
    }
    // data[0] is a texture array, data[1] the index in it.
    result = d3d11_processor_->Convert(
        reinterpret_cast<ID3D11Texture2D*>(frame->data[0]),
        static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1])),
        frame->width, frame->height, IsBt709(frame),
        frame->color_range == AVCOL_RANGE_JPEG);
    if (!result && !logged_d3d11_fallback_) {
      logged_d3d11_fallback_ = true;
      LOGW("cannot convert D3D11 frames on the GPU, downloading them");
    }
  }
#elif defined(LYCHEE_VAAPI)
  if (frame->format == AV_PIX_FMT_VAAPI && output_ &&
      (accepted_storage_ & static_cast<uint32_t>(VideoFrameStorage::kDmabuf))) {
    result = WrapDmabuf(frame);
    if (!result && !logged_dmabuf_fallback_) {
      logged_dmabuf_fallback_ = true;
      LOGW("cannot export VAAPI frames as dmabufs, downloading them");
    }
  }
#endif
  if (!result) {
    result = ConvertToCpu(frame);
  }
  if (!result) {
    return nullptr;
  }
  result->pts = frame->best_effort_timestamp == AV_NOPTS_VALUE
                    ? kNoTimestamp
                    : frame->best_effort_timestamp;
  result->duration = frame->duration > 0 ? frame->duration : 0;
  return result;
}

std::shared_ptr<VideoFrame> FFmpegVideoDecoder::ConvertToCpu(AVFrame* frame) {
  if (frame->hw_frames_ctx) {
    // A hardware frame we cannot wrap; download it first.
    ScopedAVFrame downloaded(av_frame_alloc());
    if (av_hwframe_transfer_data(downloaded.get(), frame, 0) < 0) {
      LOGE("av_hwframe_transfer_data failed");
      return nullptr;
    }
    av_frame_copy_props(downloaded.get(), frame);
    return ConvertToCpu(downloaded.get());
  }
  if (!sws_context_) {
    sws_context_ = sws_alloc_context();
  }
  auto data = std::make_shared<CpuFrameData>();
  int stride = FFALIGN(frame->width * 4, 64);
  data->data = static_cast<uint8_t*>(av_malloc(stride * frame->height));
  if (!data->data) {
    return nullptr;
  }

  ScopedAVFrame output(av_frame_alloc());
  output->format = cpu_storage_ == VideoFrameStorage::kCpuRgba
                       ? AV_PIX_FMT_RGBA
                       : AV_PIX_FMT_BGRA;
  output->width = frame->width;
  output->height = frame->height;
  output->data[0] = data->data;
  output->linesize[0] = stride;
  output->color_range = AVCOL_RANGE_JPEG;
  output->color_primaries = frame->color_primaries;
  output->color_trc = frame->color_trc;
  int ret = sws_scale_frame(sws_context_, output.get(), frame);
  if (ret < 0) {
    LOGE("sws_scale_frame: %s", AVErrorString(ret).c_str());
    return nullptr;
  }

  auto result = std::make_shared<VideoFrame>();
  result->storage = cpu_storage_;
  result->width = frame->width;
  result->height = frame->height;
  result->data = data->data;
  result->stride = stride;
  result->owner = std::move(data);
  return result;
}

void FFmpegVideoDecoder::Flush() {
  avcodec_flush_buffers(context_.get());
  consecutive_errors_ = 0;
}

}  // namespace lychee
