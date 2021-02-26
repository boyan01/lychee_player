//
// Created by boyan on 2021/2/9.
//

#include <utility>

#include "decoder_ctx.h"

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
    av_log(nullptr, AV_LOG_WARNING, "No decoder could be found for codec %s\n",
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
                                       video_render);
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
                                   audio_render);
  audio_render->Start();
  return 0;
}

DecoderContext::DecoderContext(
    std::shared_ptr<AudioRenderBase> audio_render_,
    std::shared_ptr<VideoRenderBase> video_render_,
    std::shared_ptr<ClockContext> clock_ctx_
) : audio_render(std::move(audio_render_)), video_render(std::move(video_render_)),
    clock_ctx(std::move(clock_ctx_)) {

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
