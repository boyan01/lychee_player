// ffplayer.cpp: 定义应用程序的入口点。
//

#include "ffplayer/ffplayer.h"

#include "ffplayer/ffplayer_log.h"

extern "C" {
#include "SDL2/SDL.h"
#include "libavformat/avformat.h"
#include "libavutil/time.h"
}

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0, "1", "2", 0, 0};

int packet_queue_put_private(FFPlayer *player, PacketQueue *q, AVPacket *pkt) {
    MyAVPacketList *pkt1;

    if (q->abort_request)
        return -1;

    pkt1 = (MyAVPacketList *)av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;
    if (pkt == &(player->flush_pkt))
        q->serial++;
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(q->cond);
    return 0;
}

static void packet_queue_start(FFPlayer *player, PacketQueue *q) {
    SDL_LockMutex(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(player, q, &(player->flush_pkt));
    SDL_UnlockMutex(q->mutex);
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, SDL_cond *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->empty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

static int decoder_start(FFPlayer *player, Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    packet_queue_start(player, d->queue);
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

FFPlayer::FFPlayer(const char *filename) {
    filename_ = filename;
}

void FFPlayer::OpenStream() {
    int ret;
    int st_index[AVMEDIA_TYPE_NB]{};

    auto format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    format_ctx->interrupt_callback.callback = [](void *ctx) -> int {
        auto *player = static_cast<FFPlayer *>(ctx);
        return player->abort_request_;
    };
    format_ctx->interrupt_callback.opaque = this;
    ret = avformat_open_input(&format_ctx, filename_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        LOG("can not open file" << filename_.c_str() << " code = " << ret);
        goto fail;
    }

    format_ctx_ = format_ctx;

    ret = avformat_find_stream_info(format_ctx, nullptr);
    if (ret < 0) {
        goto fail;
    }

    for (int i = 0; i < format_ctx->nb_streams; i++) {
        auto stream = format_ctx->streams[i];
        auto type = stream->codecpar->codec_type;
        stream->discard = AVDISCARD_ALL;
        if (type > 0 && wanted_stream_spec[type] && st_index[type] == -1) {
            st_index[i] = i;
        }
    }
    for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i], av_get_media_type_string(AVMediaType(i)));
            st_index[i] = INT_MAX;
        }
    }

    st_index[AVMEDIA_TYPE_VIDEO] = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO,
                                                       st_index[AVMEDIA_TYPE_VIDEO], -1, nullptr, 0);
    st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO,
                                                       st_index[AVMEDIA_TYPE_AUDIO], -1, nullptr, 0);
    OpenAudioComponentStream(st_index[AVMEDIA_TYPE_AUDIO]);
fail:
    if (format_ctx_ && !format_ctx_) {
        avformat_close_input(&format_ctx);
    }
}

int FFPlayer::OpenAudioComponentStream(int stream_index) {
    if (stream_index < 0 || stream_index >= format_ctx_->nb_streams) {
        return -1;
    }
    int ret = 0;
    auto avc_ctx = avcodec_alloc_context3(nullptr);
    if (!avc_ctx) {
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_to_context(avc_ctx, format_ctx_->streams[stream_index]->codecpar);
    if (ret < 0) {
        goto fail;
    }

    avc_ctx->pkt_timebase = format_ctx_->streams[stream_index]->time_base;
    auto codec = avcodec_find_decoder(avc_ctx->codec_id);
    if (!codec) {
        av_log(nullptr, AV_LOG_WARNING,
               "No decoder could be found for codec %s\n", avcodec_get_name(avc_ctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avc_ctx->codec_id = codec->id;
    format_ctx_->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avc_ctx->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            auto sample_rate = avc_ctx->sample_rate;
            auto nb_channels = avc_ctx->channels;
            auto channel_layout = avc_ctx->channel_layout;

            // prepare audio output.
            if ((ret = OpenAudio(channel_layout, nb_channels, sample_rate, &audio_tgt_)) < 0) {
                goto fail;
            }
            audio_hw_buf_size_ = ret;
            audio_src_ = audio_tgt_;
            audio_buf_size_ = 0;
            audio_buf_index_ = 0;

            // init averaging filter
            // TODO~

            audio_stream_index_ = stream_index;
            audio_stream_ = format_ctx_->streams[stream_index];

            decoder_init(&audio_decoder_, avc_ctx, &audio_queue_, continue_read_thread_);

            if ((ret = decoder_start(this, &audio_decoder_, audio_thread, "audio_decoder")));

            break;
        default:
            break;
    }

fail:
}

int FFPlayer::OpenAudio(int64_t wanted_channel_layout, int wanted_nb_channels,
                        int wanted_sample_rate, struct AudioParams *audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_indx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_channel_layout);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);

    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!");
        return -1;
    }
    while (next_sample_rate_indx && next_sample_rates[next_sample_rate_indx] >= wanted_spec.freq) {
        next_sample_rate_indx--;
    }
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = nullptr;
    while (!(audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_indx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(nullptr, AV_LOG_ERROR, "No more combination to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(nullptr, AV_LOG_ERROR, "SDL advised audio format %d is not supported!\n", spec.format);
    }

    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(nullptr, AV_LOG_ERROR, "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, 1, audio_hw_params->fmt, 1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->channels, audio_hw_params->freq, audio_hw_params->fmt, 1);

    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}
