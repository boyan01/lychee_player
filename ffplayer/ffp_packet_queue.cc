//
// Created by boyan on 2021/2/6.
//

#include "ffp_packet_queue.h"


AVPacket *flush_pkt;

int PacketQueue::Get(AVPacket *pkt, int block, int *pkt_serial, void *opacity, void (*on_block)(void *)) {
    MyAVPacketList *pkt1;
    int ret;

    SDL_LockMutex(mutex);

    for (;;) {
        if (abort_request) {
            ret = -1;
            break;
        }

        pkt1 = first_pkt;
        if (pkt1) {
            first_pkt = pkt1->next;
            if (!first_pkt)
                last_pkt = nullptr;
            nb_packets--;
            size -= pkt1->pkt.size + (int) sizeof(*pkt1);
            duration -= pkt1->pkt.duration;
            *pkt = pkt1->pkt;
            if (pkt_serial)
                *pkt_serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            if (on_block) {
                on_block(opacity);
            }
            SDL_CondWait(cond, mutex);
        }
    }
    SDL_UnlockMutex(mutex);
    return ret;
}

void PacketQueue::Start() {
    SDL_LockMutex(mutex);

    abort_request = 0;
    Put_(flush_pkt);
    SDL_UnlockMutex(mutex);
}

void PacketQueue::Abort() {
    SDL_LockMutex(mutex);

    abort_request = 1;

    SDL_CondSignal(cond);

    SDL_UnlockMutex(mutex);
}

int PacketQueue::Put(AVPacket *pkt) {
    int ret;

    SDL_LockMutex(mutex);
    ret = Put_(pkt);
    SDL_UnlockMutex(mutex);

    if (pkt != flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

int PacketQueue::PutNullPacket(int stream_index) {
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = nullptr;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return Put(pkt);
}

int PacketQueue::Init() {
    memset(this, 0, sizeof(PacketQueue));
    mutex = SDL_CreateMutex();
    if (!mutex) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    cond = SDL_CreateCond();
    if (!cond) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    abort_request = 1;
    return 0;
}

void PacketQueue::Flush() {
    MyAVPacketList *pkt, *pkt1;

    SDL_LockMutex(mutex);
    for (pkt = first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    last_pkt = nullptr;
    first_pkt = nullptr;
    nb_packets = 0;
    size = 0;
    duration = 0;
    SDL_UnlockMutex(mutex);
}

void PacketQueue::Destroy() {
    Flush();
    SDL_DestroyMutex(mutex);
    SDL_DestroyCond(cond);
}

int PacketQueue::Put_(AVPacket *pkt) {
    MyAVPacketList *pkt1;

    if (abort_request)
        return -1;

    pkt1 = (MyAVPacketList *) av_malloc(sizeof(MyAVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = nullptr;
    if (pkt == flush_pkt) {
        serial++;
    }
    pkt1->serial = serial;

    if (!last_pkt)
        first_pkt = pkt1;
    else
        last_pkt->next = pkt1;
    last_pkt = pkt1;
    nb_packets++;
    size += pkt1->pkt.size + (int) sizeof(*pkt1);
    duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    SDL_CondSignal(cond);
    return 0;
}
