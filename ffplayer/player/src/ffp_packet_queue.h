//
// Created by boyan on 2021/1/23.
//

#ifndef FFP_PACKET_QUEUE_H
#define FFP_PACKET_QUEUE_H

#include <mutex>
#include <condition_variable>

extern "C" {
#include "libavcodec/avcodec.h"
};

namespace media {

struct MyAVPacketList {
  AVPacket pkt;
  struct MyAVPacketList *next;
  int serial;
};

class PacketQueue {
 public:
  static AVPacket *GetFlushPacket();

  MyAVPacketList *first_pkt = nullptr, *last_pkt = nullptr;
  int nb_packets = 0;
  int size = 0;
  int64_t duration = 0;
  int abort_request = true;
  int serial = 0;
  std::mutex mutex;
  std::condition_variable_any cond;
  AVRational time_base;
 private:
  int Put_(AVPacket *pkt);

 public:

  PacketQueue();

  ~PacketQueue();

  int Put(AVPacket *pkt);

  int PutNullPacket(int stream_index);

  void Flush();

  void Abort();

  void Start();

  int Get(AVPacket *pkt, int block, int *pkt_serial, void *opacity, void (*on_block)(void *opacity));

  /**
   * Take pkt from packet queue.
   *
   * @param pkt the pkt take from queue.
   * @param pkt_serial the serial of pkt.
   * @return -1 if we got nothing, 0 if success.
   */
  int DequeuePacket(AVPacket &pkt, int *pkt_serial);

};

}

#endif //FFP_PACKET_QUEUE_H
