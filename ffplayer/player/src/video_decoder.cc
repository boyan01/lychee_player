//
// Created by yangbin on 2021/5/3.
//

#include "video_decoder.h"

#include "base/logging.h"

#include "ffp_utils.h"

extern "C" {
#include "libavutil/pixdesc.h"
}

namespace {

#if 1
enum AVPixelFormat lychee_avcodec_get_format(struct AVCodecContext *avctx,
                                             const enum AVPixelFormat *fmt) {
  const AVPixFmtDescriptor *desc;
  const AVCodecHWConfig *config;
  int i, n;

  // If a device was supplied when the codec was opened, assume that the
  // user wants to use it.
  if (avctx->hw_device_ctx && avctx->codec->hw_configs) {
    AVHWDeviceContext *device_ctx =
        (AVHWDeviceContext *) avctx->hw_device_ctx->data;
    for (i = 0;; i++) {
      config = avcodec_get_hw_config(avctx->codec, i);
      if (!config)
        break;
      if (!(config->methods &
          AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
        continue;
      if (device_ctx->type != config->device_type)
        continue;
      for (n = 0; fmt[n] != AV_PIX_FMT_NONE; n++) {
        if (config->pix_fmt == fmt[n])
          return fmt[n];
      }
    }
  }
  // No device or other setup, so we have to choose from things which
  // don't any other external information.

  // If the last element of the list is a software format, choose it
  // (this should be best software format if any exist).
  for (n = 0; fmt[n] != AV_PIX_FMT_NONE; n++);
  desc = av_pix_fmt_desc_get(fmt[n - 1]);
  if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
    return fmt[n - 1];

  // Finally, traverse the list in order and choose the first entry
  // with no external dependencies (if there is no hardware configuration
  // information available then this just picks the first entry).
  for (n = 0; fmt[n] != AV_PIX_FMT_NONE; n++) {
    for (i = 0;; i++) {
      config = avcodec_get_hw_config(avctx->codec, i);
      if (!config)
        break;
      if (config->pix_fmt == fmt[n])
        break;
    }
    if (!config) {
      // No specific config available, so the decoder must be able
      // to handle this format without any additional setup.
      return fmt[n];
    }
    if (config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) {
      // Usable with only internal setup.
      return fmt[n];
    }
  }

  // Nothing is usable, give up.
  return AV_PIX_FMT_NONE;
}
#endif
}

namespace media {

VideoDecoder::VideoDecoder() : hw_device_context_(nullptr) {};

VideoDecoder::~VideoDecoder() {
  if (hw_device_context_) {
    av_buffer_unref(&hw_device_context_);
  }
  DCHECK(!hw_device_context_);
};

int VideoDecoder::Initialize(VideoDecodeConfig config,
                             DemuxerStream *stream,
                             VideoDecoder::OutputCallback output_callback) {
  DCHECK(!codec_context_);
  DCHECK(output_callback);

  codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));
  output_callback_ = std::move(output_callback);

  auto ret = avcodec_parameters_to_context(codec_context_.get(), &config.codec_parameters());
  DCHECK_GE(ret, 0);

  codec_context_->codec_id = config.codec_id();
  codec_context_->pkt_timebase = config.time_base();

  auto *codec = avcodec_find_decoder(config.codec_id());
  DCHECK(codec) << "no decoder could be found" << avcodec_get_name(config.codec_id());

  if (codec == nullptr) {
    codec_context_.reset();
    return -1;
  }

  codec_context_->codec_id = codec->id;
  int stream_lower = config.low_res();
  DCHECK_LE(stream_lower, int(codec->max_lowres))
      << "The maximum value for lowres supported by the decoder is " << codec->max_lowres
      << ", but is " << int(codec->max_lowres);
  if (stream_lower > codec->max_lowres) {
    stream_lower = codec->max_lowres;
  }
  codec_context_->lowres = stream_lower;
  if (config.fast()) {
    codec_context_->flags2 |= AV_CODEC_FLAG2_FAST;
  }

#if defined(_MEDIA_ENABLE_HW_ACCEL)
  auto *hw_config = avcodec_get_hw_config(codec, 0);
  if (hw_config) {
    DLOG(WARNING) << "hw_config: " << hw_config->pix_fmt;
    DLOG(WARNING) << "hw_config: " << hw_config->device_type;
  }
  ret = av_hwdevice_ctx_create(&hw_device_context_, AVHWDeviceType::AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
  if (ret < 0) {
    DLOG(INFO) << "failed to create hw device_context" << av_err_to_str(ret);
  } else {
    codec_context_->hw_device_ctx = av_buffer_ref(hw_device_context_);
  }
#endif
  ret = avcodec_open2(codec_context_.get(), codec, nullptr);
  DCHECK_GE(ret, 0) << "can not open avcodec, reason: " << av_err_to_str(ret);
  if (ret < 0) {
    codec_context_.reset();
    return ret;
  }

  ffmpeg_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(codec_context_.get(), true);
//  video_render->SetMaxFrameDuration(video_decode_config_.max_frame_duration());
  video_decode_config_ = config;

  stream_ = stream;

  return 0;
}

void VideoDecoder::Decode(std::shared_ptr<DecoderBuffer> decoder_buffer) {
  DCHECK(!decoder_buffer->end_of_stream());
  switch (ffmpeg_decoding_loop_->DecodePacket(
      decoder_buffer->av_packet(), std::bind(&VideoDecoder::OnFrameAvailable, this, std::placeholders::_1))) {
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed :return;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      DLOG(ERROR) << "Failed to send video packet for decoding";
      return;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      DLOG(ERROR) << " failed to decode a video frame: "
                  << av_err_to_str(ffmpeg_decoding_loop_->last_av_error_code());
      return;
    }
    case FFmpegDecodingLoop::DecodeStatus::kOkay:break;
  }
}

bool VideoDecoder::OnFrameAvailable(AVFrame *frame) {
  auto frame_rate = video_decode_config_.frame_rate();
  auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
  auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : double(frame->pts) * av_q2d(video_decode_config_.time_base());

  std::shared_ptr<VideoFrame> video_frame = std::make_shared<VideoFrame>(frame, pts, duration, 0);
  output_callback_(std::move(video_frame));
  return false;
}

void VideoDecoder::Flush() {
  avcodec_flush_buffers(codec_context_.get());
}

} // namespace media