#define LOG_TAG "ffmpeg_video"

#include "backends/ffmpeg/ffmpeg_video_decoder.h"

#include "base/log.h"

#if defined(__APPLE__)
#include "backends/apple/pixel_buffer.h"
#endif

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace lychee {

namespace {

AVHWDeviceType PlatformHardwareDeviceType() {
#if defined(__APPLE__)
  return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
  return AV_HWDEVICE_TYPE_NONE;
#endif
}

struct CpuFrameData {
  uint8_t* data = nullptr;
  ~CpuFrameData() { av_free(data); }
};

}  // namespace

FFmpegVideoDecoder::FFmpegVideoDecoder(bool hardware)
    : hardware_(hardware),
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
  int ret = av_hwdevice_ctx_create(&hw_device_context_, hw_device_type_,
                                   nullptr, nullptr, 0);
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
#endif
  if (!result) {
    result = ConvertToBgra(frame);
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

std::shared_ptr<VideoFrame> FFmpegVideoDecoder::ConvertToBgra(AVFrame* frame) {
  if (frame->hw_frames_ctx) {
    // A hardware frame we cannot wrap; download it first.
    ScopedAVFrame downloaded(av_frame_alloc());
    if (av_hwframe_transfer_data(downloaded.get(), frame, 0) < 0) {
      LOGE("av_hwframe_transfer_data failed");
      return nullptr;
    }
    av_frame_copy_props(downloaded.get(), frame);
    return ConvertToBgra(downloaded.get());
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

  ScopedAVFrame bgra(av_frame_alloc());
  bgra->format = AV_PIX_FMT_BGRA;
  bgra->width = frame->width;
  bgra->height = frame->height;
  bgra->data[0] = data->data;
  bgra->linesize[0] = stride;
  bgra->color_range = AVCOL_RANGE_JPEG;
  bgra->color_primaries = frame->color_primaries;
  bgra->color_trc = frame->color_trc;
  int ret = sws_scale_frame(sws_context_, bgra.get(), frame);
  if (ret < 0) {
    LOGE("sws_scale_frame: %s", AVErrorString(ret).c_str());
    return nullptr;
  }

  auto result = std::make_shared<VideoFrame>();
  result->storage = VideoFrameStorage::kCpuBgra;
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
