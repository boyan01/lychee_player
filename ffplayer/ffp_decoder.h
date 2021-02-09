//
// Created by boyan on 2021/2/9.
//

#ifndef FFPLAYER_FFP_DECODER_H
#define FFPLAYER_FFP_DECODER_H

#include "ffp_packet_queue.h"

extern "C" {
#include "libavcodec/avcodec.h"
};

typedef struct Decoder {
    AVPacket pkt{0};
    PacketQueue *queue = nullptr;
    AVCodecContext *avctx = nullptr;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;

public:
    void Init(AVCodecContext *av_codec_ctx, PacketQueue *_queue, SDL_cond *_empty_queue_cond);

} Decoder;


#endif //FFPLAYER_FFP_DECODER_H
