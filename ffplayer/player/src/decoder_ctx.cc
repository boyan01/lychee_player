//
// Created by boyan on 2021/2/9.
//

#include <utility>

#include "base/logging.h"

#include "decoder_ctx.h"

namespace media {

int DecoderContext::StartDecoder(std::unique_ptr<DecodeParams> decode_params) {
  unique_ptr_d<AVCodecContext> codec_ctx(avcodec_alloc_context3(nullptr),
                                         [](AVCodecContext *ptr) {
                                           avcodec_free_context(&ptr);
                                         });
  auto *stream = decode_params->stream();
  if (!stream) {
    return -1;
  }
  if (!codec_ctx) {
    return AVERROR(ENOMEM);
  }
  int ret;
  ret = avcodec_parameters_to_context(codec_ctx.get(), stream->codecpar);
  if (ret < 0) {
    return ret;
  }
  codec_ctx->pkt_timebase = stream->time_base;

  auto codec = avcodec_find_decoder(codec_ctx->codec_id);
  if (!codec) {
    av_log(nullptr, AV_LOG_WARNING, "No decoder could be found for CodecId %s\n",
           avcodec_get_name(codec_ctx->codec_id));
    return AVERROR(EINVAL);
  }

  codec_ctx->codec_id = codec->id;
  auto stream_lowers = this->low_res;
  if (stream_lowers > codec->max_lowres) {
    av_log(codec_ctx.get(), AV_LOG_WARNING,
           "The maximum value for lowres supported by the decoder is %d, but set %d.\n",
           codec->max_lowres, stream_lowers);
    stream_lowers = codec->max_lowres;
  }
  codec_ctx->lowres = stream_lowers;
  if (fast) {
    codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;
  }

  ret = avcodec_open2(codec_ctx.get(), codec, nullptr);
  if (ret < 0) {
    return ret;
  }

  stream->discard = AVDISCARD_DEFAULT;
  switch (codec_ctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
      video_decoder = new VideoDecoder(std::move(codec_ctx), std::move(decode_params),
                                       video_render, on_decoder_blocking_);
      break;
    case AVMEDIA_TYPE_AUDIO: {
      return StartAudioDecoder(std::move(codec_ctx), std::move(decode_params));
    }
    case AVMEDIA_TYPE_SUBTITLE:
      av_log(nullptr,
             AV_LOG_WARNING,
             "source contains subtitle, but subtitle render do not supported. \n");
      break;
    default:break;
  }
  return 0;
}

int DecoderContext::StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx,
                                      std::unique_ptr<DecodeParams> decode_params) {
  CHECK_VALUE_WITH_RETURN(audio_render, -1);
  int sample_rate, nb_channels;
  int64_t channel_layout;
#if CONFIG_AVFILTER
  {
              AVFilterContext *sink;

              is->audio_filter_src.freq = avctx->sample_rate;
              is->audio_filter_src.channels = avctx->channels;
              is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
              is->audio_filter_src.fmt = avctx->sample_fmt;
              if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                  goto fail;
              sink = is->out_audio_filter;
              sample_rate = av_buffersink_get_sample_rate(sink);
              nb_channels = av_buffersink_get_channels(sink);
              channel_layout = av_buffersink_get_channel_layout(sink);
          }
#else
  sample_rate = codec_ctx->sample_rate;
  nb_channels = codec_ctx->channels;
  channel_layout = codec_ctx->channel_layout;
#endif
  auto ret = audio_render->Open(channel_layout, nb_channels, sample_rate);
  if (ret <= 0) {
    return -1;
  }

  audio_decoder = new AudioDecoder(std::move(codec_ctx), std::move(decode_params),
                                   audio_render, on_decoder_blocking_);
  audio_render->Start();
  return 0;
}

DecoderContext::DecoderContext(
    std::shared_ptr<BasicAudioRender> audio_render_,
    std::shared_ptr<VideoRenderBase> video_render_,
    std::shared_ptr<MediaClock> clock_ctx_,
    std::function<void()> on_decoder_blocking
) : audio_render(std::move(audio_render_)),
    video_render(std::move(video_render_)),
    video_decode_config_(),
    clock_ctx(std::move(clock_ctx_)),
    on_decoder_blocking_(std::move(on_decoder_blocking)) {
  decode_task_runner_ = TaskRunner::prepare_looper("decoder");
}

DecoderContext::~DecoderContext() {
  if (audio_decoder) {
    audio_decoder->Abort(nullptr);
  }
  if (video_decoder) {
    video_decoder->Abort(nullptr);
  }
  if (video_render) {
    video_render->Abort();
  }
  if (audio_decoder) {
    audio_decoder->Join();
    delete audio_decoder;
  }
  if (video_decoder) {
    video_decoder->Join();
    delete video_decoder;
  }
}

int DecoderContext::InitVideoDecoder(VideoDecodeConfig config) {
  DCHECK(!video_codec_context_);

  video_codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));

  auto ret = avcodec_parameters_to_context(video_codec_context_.get(), &config.codec_parameters());
  DCHECK_GE(ret, 0);

  video_codec_context_->codec_id = config.codec_id();
  video_codec_context_->pkt_timebase = config.time_base();

  auto *codec = avcodec_find_decoder(config.codec_id());
  DCHECK(codec) << "No decoder could be found for CodecId:" << avcodec_get_name(config.codec_id());

  if (codec == nullptr) {
    video_codec_context_.reset();
    return -1;
  }

  video_codec_context_->codec_id = codec->id;
  int stream_lower = config.low_res();
  DCHECK_LE(stream_lower, codec->max_lowres)
      << "The maximum value for lowres supported by the decoder is " << codec->max_lowres
      << ", but is " << stream_lower;
  if (stream_lower > codec->max_lowres) {
    stream_lower = codec->max_lowres;
  }
  video_codec_context_->lowres = stream_lower;
  if (config.fast()) {
    video_codec_context_->flags2 |= AV_CODEC_FLAG2_FAST;
  }

  ret = avcodec_open2(video_codec_context_.get(), codec, nullptr);
  DCHECK_GE(ret, 0) << "can not open avcodec, reason: " << av_err_to_str(ret);
  if (ret < 0) {
    video_codec_context_.reset();
    return ret;
  }

  video_temp_frame_ = av_frame_alloc();
  DCHECK(video_temp_frame_);

  video_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(video_codec_context_.get(), true);
  video_render->SetMaxFrameDuration(video_decode_config_.max_frame_duration());
  video_decode_config_ = config;

  return 0;
}

void DecoderContext::StartVideoDecoder(std::shared_ptr<DemuxerStream> stream) {
  DCHECK(video_codec_context_);

  video_stream_ = std::move(stream);

  video_stream_->stream()->discard = AVDISCARD_DEFAULT;
  video_stream_->packet_queue()->Start();

  decode_task_runner_->PostTask(FROM_HERE, [&]() {
    VideoDecodeTask();
  });
}

void DecoderContext::VideoDecodeTask() {
  DCHECK(decode_task_runner_->BelongsToCurrentThread());
  DCHECK(video_stream_);
  DCHECK(video_temp_frame_);
  DCHECK(video_decoding_loop_);

  AVPacket packet;
  av_init_packet(&packet);
  if (!video_stream_->ReadPacket(&packet)) {
    goto end;
  }

  switch (video_decoding_loop_->DecodePacket(&packet, [&](AVFrame *frame) {
    auto frame_rate = video_decode_config_.frame_rate();
    auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
    auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(video_decode_config_.time_base());
    video_render->PushFrame(frame, pts, duration, 0);
    return false;
  })) {
    case FFmpegDecodingLoop::DecodeStatus::kOkay:break;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed:break;
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed:break;
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed:break;
  }

  end:
  decode_task_runner_->PostTask(FROM_HERE, [&]() {
    VideoDecodeTask();
  });
}

}
