//
// Created by boyan on 2021/2/9.
//

#ifndef FFPLAYER_FFP_DECODER_H
#define FFPLAYER_FFP_DECODER_H

#include <functional>
#include <condition_variable>

#include "ffp_packet_queue.h"
#include "ffp_frame_queue.h"
#include "ffp_define.h"
#include "ffp_audio_render.h"
#include "ffp_video_render.h"
#include "render_base.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
};


struct DecodeParams {
    std::shared_ptr<PacketQueue> pkt_queue;
    std::shared_ptr<std::condition_variable_any> read_condition;
    AVFormatContext *const *format_ctx;
    int stream_index = -1;
    bool audio_follow_stream_start_pts;
public:
    DecodeParams(std::shared_ptr<PacketQueue> pkt_queue_,
                 std::shared_ptr<std::condition_variable_any> read_condition_,
                 AVFormatContext *const *format_ctx_,
                 int stream_index_
    ) : pkt_queue(std::move(pkt_queue_)),
        read_condition(read_condition_),
        format_ctx(format_ctx_),
        stream_index(stream_index_) {
    }

    AVStream *stream() {
        if (!*format_ctx) {
            return nullptr;
        } else {
            return (*format_ctx)->streams[stream_index];
        }
    }

};

template<class T>
class Decoder {
    static_assert(std::is_base_of<BaseRender, T>::value, "T must inherit from BaseRender");
public:
    AVPacket pkt{0};
    unique_ptr_d<AVCodecContext> avctx;
    int pkt_serial = -1;
    int finished = 0;
    int packet_pending = 0;
    int64_t start_pts = AV_NOPTS_VALUE;
    AVRational start_pts_tb{0};
    int64_t next_pts = 0;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid = nullptr;
    int decoder_reorder_pts = -1;
    std::function<void()> on_frame_decode_block = nullptr;
    std::shared_ptr<T> render;
    std::unique_ptr<DecodeParams> decode_params;

private:

    int Start(int (*fn)(void *), void *arg) {
        decode_params->pkt_queue->Start();
        decoder_tid = SDL_CreateThread(fn, debug_label(), arg);
        if (!decoder_tid) {
            av_log(nullptr, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
            return AVERROR(ENOMEM);
        }
        return 0;
    }

protected:

    const std::shared_ptr<PacketQueue> &queue() {
        return decode_params->pkt_queue;
    }

    const std::shared_ptr<std::condition_variable_any> &empty_queue_cond() {
        return decode_params->read_condition;
    }

    int DecodeFrame(AVFrame *frame, AVSubtitle *sub);

    virtual const char *debug_label() = 0;

    virtual int DecodeThread() = 0;

    void Start() {
        Start([](void *arg) {
            auto *decoder = static_cast<Decoder<T> *>(arg);
            return decoder->DecodeThread();
        }, this);
    }

public:

    Decoder(unique_ptr_d<AVCodecContext> codec_context,
            std::unique_ptr<DecodeParams> decode_params_,
            std::shared_ptr<T> render_)
            : decode_params(std::move(decode_params_)), avctx(std::move(codec_context)),
              render(std::move(render_)) {
    }

    ~Decoder() {
        queue()->Abort();
        render->Abort();
        SDL_WaitThread(decoder_tid, nullptr);
        queue()->Flush();
    }

    void Abort(FrameQueue *fq) {
        auto d = this;
        d->queue->Abort();
        fq->Signal();
        SDL_WaitThread(d->decoder_tid, nullptr);
        d->decoder_tid = nullptr;
        d->queue->Flush();
    }

};


class DecoderContext {
public:
    /**
     * low resolution decoding, 1-> 1/2 size, 2->1/4 size
     */
    int low_res = 0;
    bool fast = false;

    int frame_drop_count = 0;

private:
    std::unique_ptr<Decoder<AudioRender>> audio_decoder;
    std::unique_ptr<Decoder<VideoRender>> video_decoder;
//    Decoder<void> *subtitle_decoder = nullptr;

    std::shared_ptr<AudioRender> audio_render;
    std::shared_ptr<VideoRender> video_render;

    std::shared_ptr<ClockContext> clock_ctx;

private:
    int StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx, std::unique_ptr<DecodeParams> decode_params);

public:

    DecoderContext(std::shared_ptr<AudioRender> audio_render_, std::shared_ptr<VideoRender> video_render_,
                   std::shared_ptr<ClockContext> clock_ctx_);

    ~DecoderContext();

    int StartDecoder(std::unique_ptr<DecodeParams> decode_params);

};


#endif //FFPLAYER_FFP_DECODER_H
