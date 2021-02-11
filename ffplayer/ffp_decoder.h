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

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
};


struct DecodeParams {
    AVStream *stream = nullptr;
    PacketQueue *pkt_queue = nullptr;
    std::condition_variable_any *read_condition = nullptr;
    bool audio_follow_stream_start_pts;
};

typedef struct Decoder {
    AVPacket pkt{0};
    PacketQueue *queue = nullptr;
    AVCodecContext *avctx = nullptr;
    int pkt_serial = 0;
    int finished = 0;
    int packet_pending = 0;
    std::condition_variable_any *empty_queue_cond;
    int64_t start_pts = 0;
    AVRational start_pts_tb{0};
    int64_t next_pts = 0;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid = nullptr;
    int decoder_reorder_pts = -1;
    std::function<void()> on_frame_decode_block = nullptr;

public:
    void Init(AVCodecContext *av_codec_ctx, PacketQueue *_queue, std::condition_variable_any *_empty_queue_cond);

    void Destroy() {
        av_packet_unref(&pkt);
        avcodec_free_context(&avctx);
    }

    void Abort(FrameQueue *fq) {
        auto d = this;
        d->queue->Abort();
        fq->Signal();
        SDL_WaitThread(d->decoder_tid, nullptr);
        d->decoder_tid = nullptr;
        d->queue->Flush();
    }

} Decoder;


int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub);

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    d->queue->Start();
    d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
    if (!d->decoder_tid) {
        av_log(nullptr, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

class DecoderContext {
public:
    /**
     * low resolution decoding, 1-> 1/2 size, 2->1/4 size
     */
    int low_res = 0;
    bool fast = false;
    AudioRender *audio_render;

    Decoder *audio_decoder;
    Decoder *video_decoder;
    Decoder *subtitle_decoder;


private:
    int StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx, const DecodeParams *decode_params);

    int AudioThread() const;

public:
    int StartDecoder(const DecodeParams *decode_params);

};


#endif //FFPLAYER_FFP_DECODER_H
