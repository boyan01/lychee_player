//
// Created by boyan on 2021/1/23.
//

#ifndef FFPLAYER_FFPLAYER_PACKET_QUEUE_H
#define FFPLAYER_FFPLAYER_PACKET_QUEUE_H

extern "C" {
#include "SDL2/SDL_mutex.h"
#include "libavcodec/avcodec.h"
};

typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
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
