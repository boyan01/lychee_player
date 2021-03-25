//
// Created by boyan on 2021/2/6.
//

#include "ffp_packet_queue.h"

namespace media {

int PacketQueue::Get(AVPacket *pkt, int block, int *pkt_serial, void *opacity, void (*on_block)(void *)) {
  MyAVPacketList *pkt1;
  int ret;

  std::unique_lock<std::mutex> lock(mutex);

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
      cond.wait(lock);
    }
  }
  return ret;
}

void PacketQueue::Start() {
  abort_request = 0;
  Put_(GetFlushPacket());
}

void PacketQueue::Abort() {
  std::lock_guard<std::mutex> lock(mutex);

  abort_request = 1;
  cond.notify_all();
}

int PacketQueue::Put(AVPacket *pkt) {
  int ret;

  ret = Put_(pkt);

  if (pkt != GetFlushPacket() && ret < 0)
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

PacketQueue::PacketQueue() {
  abort_request = 1;
}

PacketQueue::~PacketQueue() {
  Flush();
  Abort();
}

void PacketQueue::Flush() {
  std::lock_guard<std::mutex> lock(mutex);

  MyAVPacketList *pkt, *pkt1;

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
}

int PacketQueue::Put_(AVPacket *pkt) {
  std::lock_guard<std::mutex> lock(mutex);

  MyAVPacketList *pkt1;

  if (abort_request)
    return -1;

  pkt1 = (MyAVPacketList *) av_malloc(sizeof(MyAVPacketList));
  if (!pkt1)
    return -1;
  pkt1->pkt = *pkt;
  pkt1->next = nullptr;
  if (pkt == GetFlushPacket()) {
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
  cond.notify_all();
  return 0;
}

int PacketQueue::DequeuePacket(AVPacket &pkt, int *pkt_serial) {
  std::lock_guard<std::mutex> lck(mutex);
  if (abort_request) {
    return -1;
  }
  MyAVPacketList *head = first_pkt;
  if (!head) {
    return -1;
  }

  first_pkt = head->next;
  if (!first_pkt) {
    last_pkt = nullptr;
  }
  nb_packets--;
  size -= head->pkt.size + int(sizeof(*head));
  duration -= head->pkt.duration;

  pkt = head->pkt;
  if (pkt_serial) {
    *pkt_serial = head->serial;
  }
  av_free(head);
  return 0;
}

AVPacket *PacketQueue::GetFlushPacket() {
  static AVPacket flush_pkt;
  av_init_packet(&flush_pkt);
  flush_pkt.data = (uint8_t *) &flush_pkt;
  return &flush_pkt;
}

}