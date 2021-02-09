//
// Created by boyan on 2021/2/9.
//

#include "ffp_decoder.h"

extern "C" {
}

void Decoder::Init(AVCodecContext *av_codec_ctx, PacketQueue *_queue, SDL_cond *_empty_queue_cond) {
    memset(this, 0, sizeof(Decoder));
    avctx = av_codec_ctx;
    queue = _queue;
    empty_queue_cond = _empty_queue_cond;
    start_pts = AV_NOPTS_VALUE;
    pkt_serial = -1;
}
