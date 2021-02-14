//
// Created by boyan on 2021/2/9.
//

#include "ffp_decoder.h"

#include <utility>

extern AVPacket *flush_pkt;

class VideoDecoder : public Decoder<VideoRender> {

private:

    int GetVideoFrame(AVFrame *frame) {
        auto got_picture = DecodeFrame(frame, nullptr);
        if (got_picture < 0) {
            return -1;
        }
        return got_picture;
    }

protected:

    const char *debug_label() override {
        return "video_decoder";
    }

    int DecodeThread() override {
        if (*decode_params->format_ctx == nullptr) {
            return -1;
        }

        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            return AVERROR(ENOMEM);
        }
        int ret;
        AVRational tb = decode_params->stream()->time_base;
        AVRational frame_rate = av_guess_frame_rate(*decode_params->format_ctx, decode_params->stream(), nullptr);
        for (;;) {
            ret = GetVideoFrame(frame);
            if (ret < 0) {
                break;
            }
            if (!ret) {
                continue;
            }
            auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
            auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);

            ret = render->PushFrame(frame, pts, duration, pkt_serial);
            av_frame_unref(frame);
            if (ret < 0) {
                break;
            }
        }
        av_frame_free(&frame);
        return 0;
    }

public:
    VideoDecoder(unique_ptr_d<AVCodecContext> codecContext,
                 std::unique_ptr<DecodeParams> decodeParams,
                 std::shared_ptr<VideoRender> render)
            : Decoder(std::move(codecContext), std::move(decodeParams), std::move(render)) {
        Start();
    }
};

class AudioDecoder : public Decoder<AudioRender> {

protected:

    const char *debug_label() override {
        return "audio_decoder";
    }


    int DecodeThread() override {
        AVFrame *frame = av_frame_alloc();
        if (!frame) {
            return AVERROR(ENOMEM);
        }
        render->audio_queue_serial = &queue()->serial;
        do {
            auto got_frame = DecodeFrame(frame, nullptr);
            if (got_frame < 0) {
                break;
            }
            if (got_frame) {
                if (render->PushFrame(frame, pkt_serial) < 0) {
                    break;
                }
            }
        } while (true);
        av_frame_free(&frame);
        return 0;
    }

public:
    AudioDecoder(unique_ptr_d<AVCodecContext> codec_context_,
                 std::unique_ptr<DecodeParams> decode_params_,
                 const std::shared_ptr<AudioRender> &render_)
            : Decoder(std::move(codec_context_),
                      std::move(decode_params_),
                      render_) {
        if (decode_params->audio_follow_stream_start_pts) {
            start_pts = decode_params->stream()->start_time;
            start_pts_tb = decode_params->stream()->time_base;
        }
        Start();
    }
};


template<class T>
int Decoder<T>::DecodeFrame(AVFrame *frame, AVSubtitle *sub) {
    auto *d = this;
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket temp_pkt;

        if (d->queue()->serial == d->pkt_serial) {
            do {
                if (d->queue()->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx.get(), frame);
                        if (ret >= 0) {
                            if (d->decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!d->decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx.get(), frame);
                        if (ret >= 0) {
                            AVRational tb = {1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                    default:
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx.get());
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue()->nb_packets == 0) {
                d->empty_queue_cond()->notify_all();
            }
            if (d->packet_pending) {
                av_packet_move_ref(&temp_pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (d->queue()->Get(&temp_pkt, 1, &d->pkt_serial, d, [](void *opacity) {
                    auto decoder = static_cast<Decoder *>(opacity);
                    if (decoder->on_frame_decode_block) {
                        decoder->on_frame_decode_block();
                    }
                }) < 0) {
                    return -1;
                }
            }
            if (d->queue()->serial == d->pkt_serial)
                break;
            av_packet_unref(&temp_pkt);
        } while (true);

        if (temp_pkt.data == flush_pkt->data) {
            avcodec_flush_buffers(d->avctx.get());
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx.get(), sub, &got_frame, &temp_pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !temp_pkt.data) {
                        d->packet_pending = 1;
                        av_packet_move_ref(&d->pkt, &temp_pkt);
                    }
                    ret = got_frame ? 0 : (temp_pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx.get(), &temp_pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx.get(), AV_LOG_ERROR,
                           "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &temp_pkt);
                }
            }
            av_packet_unref(&temp_pkt);
        }
    }
}


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
            video_decoder = std::make_unique<VideoDecoder>(std::move(codec_ctx), std::move(decode_params),
                                                           std::move(video_render));
            break;
        case AVMEDIA_TYPE_AUDIO:
            StartAudioDecoder(std::move(codec_ctx), std::move(decode_params));
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            av_log(nullptr,
                   AV_LOG_WARNING,
                   "source contains subtitle, but subtitle render do not supported. \n");
            break;
        default:
            break;
    }
    return 0;
}

int DecoderContext::StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx,
                                      std::unique_ptr<DecodeParams> decode_params) {
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

    audio_decoder = std::make_unique<AudioDecoder>(std::move(codec_ctx), std::move(decode_params),
                                                   std::move(audio_render));
    audio_render->Start();
    return 0;
}


DecoderContext::DecoderContext(std::shared_ptr<AudioRender> audio_render_, std::shared_ptr<VideoRender> video_render_,
                               std::shared_ptr<ClockContext> clock_ctx_)
        : audio_render(std::move(audio_render_)), video_render(std::move(video_render_)),
          clock_ctx(std::move(clock_ctx_)) {

}

DecoderContext::~DecoderContext() {
}
