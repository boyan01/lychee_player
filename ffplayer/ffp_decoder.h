//
// Created by boyan on 2021/2/9.
//

#ifndef FFPLAYER_FFP_DECODER_H
#define FFPLAYER_FFP_DECODER_H

#include "functional"

#include "ffp_packet_queue.h"
#include "ffp_frame_queue.h"

extern "C" {
#include "libavcodec/avcodec.h"
};

typedef struct Decoder {
    AVPacket pkt{0};
    PacketQueue *queue = nullptr;
    AVCodecContext *avctx = nullptr;
    int pkt_serial = 0;
    int finished = 0;
    int packet_pending = 0;
    SDL_cond *empty_queue_cond;
    int64_t start_pts = 0;
    AVRational start_pts_tb{0};
    int64_t next_pts = 0;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid = nullptr;
    int decoder_reorder_pts = -1;
    std::function<void()> on_frame_decode_block = nullptr;

public:
    void Init(AVCodecContext *av_codec_ctx, PacketQueue *_queue, SDL_cond *_empty_queue_cond);

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


#endif //FFPLAYER_FFP_DECODER_H
