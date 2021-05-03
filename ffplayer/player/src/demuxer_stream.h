//
// Created by yangbin on 2021/4/19.
//

#ifndef MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
#define MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_

#include "base/basictypes.h"

extern "C" {
#include "libavformat/avformat.h"
};

#include "ffp_packet_queue.h"
#include "audio_decode_config.h"
#include "video_decode_config.h"

namespace media {

class DemuxerStream {

 public:
  enum Type {
    UNKNOWN,
    Audio,
    Video,
    NUM_TYPES,  // Always keep this entry as the last one!
  };

  DemuxerStream(AVStream *stream,
                PacketQueue *packet_queue,
                Type type,
                std::unique_ptr<AudioDecodeConfig> audio_decode_config,
                std::unique_ptr<VideoDecodeConfig> video_decode_config);

  AVStream *stream() const {
    return stream_;
  }

  PacketQueue *packet_queue() const {
    return packet_queue_;
  }

  bool ReadPacket(AVPacket *packet) {
    auto ret = packet_queue_->Get(packet, true, nullptr, nullptr, nullptr);
    return ret > 0;
  }

  AudioDecodeConfig audio_decode_config();

  VideoDecodeConfig video_decode_config();

 private:

  AVStream *stream_;
  PacketQueue *packet_queue_;
  std::unique_ptr<AudioDecodeConfig> audio_decode_config_;
  std::unique_ptr<VideoDecodeConfig> video_decode_config_;
  Type type_;

  DISALLOW_COPY_AND_ASSIGN(DemuxerStream);

};

} // namespace media

#endif //MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
