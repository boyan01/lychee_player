//
// Created by yangbin on 2021/4/19.
//

#ifndef MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
#define MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_

extern "C" {
#include "libavformat/avformat.h"
};

#include "ffp_packet_queue.h"

namespace media {

class DemuxerStream {

 public:

  DemuxerStream(AVStream *stream, PacketQueue *packet_queue) : stream_(stream), packet_queue_(packet_queue) {}

  AVStream *stream() const {
    return stream_;
  }

  PacketQueue *packet_queue() const {
    return packet_queue_;
  }

  bool ReadPacket(AVPacket *packet) {
    auto ret = packet_queue_->Get(packet, true, nullptr, nullptr, nullptr);
    return ret >= 0;
  }

 private:

  AVStream *stream_;
  PacketQueue *packet_queue_;

};

} // namespace media

#endif //MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
