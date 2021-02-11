//
// Created by boyan on 2021/2/9.
//

#include "ffp_decoder.h"
#include "ffp_define.h"

extern "C" {
}

extern AVPacket *flush_pkt;

void Decoder::Init(AVCodecContext *av_codec_ctx, PacketQueue *_queue, std::condition_variable_any *_empty_queue_cond) {
    memset(this, 0, sizeof(Decoder));
    avctx = av_codec_ctx;
    queue = _queue;
    empty_queue_cond = _empty_queue_cond;
    start_pts = AV_NOPTS_VALUE;
    pkt_serial = -1;
}

int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (d->decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!d->decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
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
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0) {
                d->empty_queue_cond->notify_all();
            }
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (d->queue->Get(&pkt, 1, &d->pkt_serial, d, [](void *opacity) {
                    auto decoder = static_cast<Decoder *>(opacity);
                    if (decoder->on_frame_decode_block) {
                        decoder->on_frame_decode_block();
                    }
                }) < 0) {
                    return -1;
                }
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(&pkt);
        } while (true);

        if (pkt.data == flush_pkt->data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                        d->packet_pending = 1;
                        av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    av_log(d->avctx, AV_LOG_ERROR,
                           "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }
}

int DecoderContext::StartDecoder(const DecodeParams *decode_params) {
    unique_ptr_d<AVCodecContext> codec_ctx(avcodec_alloc_context3(nullptr),
                                           [](AVCodecContext *ptr) {
                                               avcodec_free_context(&ptr);
                                           });
    auto stream = decode_params->stream;
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
            break;
        case AVMEDIA_TYPE_AUDIO:
            StartAudioDecoder(std::move(codec_ctx), decode_params);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            break;
        default:
            break;
    }
    return 0;
}

int
DecoderContext::StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx, const DecodeParams *decode_params) {
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

    audio_decoder->Init(codec_ctx.release(), decode_params->pkt_queue, decode_params->read_condition);
    if (decode_params->audio_follow_stream_start_pts) {
        audio_decoder->start_pts = decode_params->stream->start_time;
        audio_decoder->start_pts_tb = decode_params->stream->time_base;
    }
    if (decoder_start(audio_decoder, [](void *arg) -> int {
        auto *ctx = static_cast<DecoderContext *>(arg);
        ctx->AudioThread();
        return -1;
    }, "audio_thread", this) < 0) {
        return -1;
    }
    audio_render->Start();
    return 0;
}

int DecoderContext::AudioThread() const {
//    unique_ptr_d<AVFrame> frame(av_frame_alloc(), [](AVFrame *ptr) {
//        av_frame_free(&ptr);
//    });
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        return AVERROR(ENOMEM);
    }
    audio_render->audio_queue_serial = &audio_decoder->queue->serial;
    do {
        auto got_frame = decoder_decode_frame(audio_decoder, frame, nullptr);
        if (got_frame < 0) {
            break;
        }
        if (got_frame) {
            if (audio_render->PushFrame(frame, audio_decoder->pkt_serial) < 0) {
                break;
            }
        }
    } while (true);
    av_frame_free(&frame);
    return 0;
}
