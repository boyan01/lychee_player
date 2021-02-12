//
// Created by boyan on 2021/1/23.
//

#ifndef FFPLAYER_FFPLAYER_PACKET_QUEUE_H
#define FFPLAYER_FFPLAYER_PACKET_QUEUE_H

extern "C" {
#include "SDL2/SDL.h"
#include "libavcodec/avcodec.h"
};

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt = nullptr;
    int nb_packets = 0;
    int size = 0;
    int64_t duration = 0;
    int abort_request = true;
    int serial = 0;
    SDL_mutex *mutex = nullptr;
    SDL_cond *cond = nullptr;
private:
    int Put_(AVPacket *pkt);

public:

    int Put(AVPacket *pkt);

    int PutNullPacket(int stream_index);

    int Init();

    void Flush();

    void Destroy();

    void Abort();

    void Start();

    int Get(AVPacket *pkt, int block, int *pkt_serial, void *opacity, void (*on_block)(void *opacity));

} PacketQueue;


#endif //FFPLAYER_FFPLAYER_PACKET_QUEUE_H
