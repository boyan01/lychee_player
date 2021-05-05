//
// Created by yangbin on 2021/4/19.
//

#include "demuxer_stream.h"

#include "base/logging.h"

namespace media {

DemuxerStream::DemuxerStream(
    AVStream *stream,
    PacketQueue *packet_queue,
    Type type,
    std::unique_ptr<AudioDecodeConfig> audio_decode_config,
    std::unique_ptr<VideoDecodeConfig> video_decode_config,
    std::shared_ptr<std::condition_variable_any> continue_read_thread
) : stream_(stream),
    packet_queue_(packet_queue),
    type_(type),
    audio_decode_config_(std::move(audio_decode_config)),
    video_decode_config_(std::move(video_decode_config)),
    continue_read_thread_(std::move(continue_read_thread)) {

}

AudioDecodeConfig DemuxerStream::audio_decode_config() {
  DCHECK_EQ(type_, Audio);
  return *audio_decode_config_;
}

VideoDecodeConfig DemuxerStream::video_decode_config() {
  DCHECK_EQ(type_, Video);
  return *video_decode_config_;
}

} // namespace media