//
// Created by yangbin on 2021/4/19.
//

#ifndef MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
#define MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_

#include "functional"

#include "base/basictypes.h"
#include "base/callback.h"

extern "C" {
#include "libavformat/avformat.h"
}

#include "ffp_packet_queue.h"
#include "audio_decode_config.h"
#include "video_decode_config.h"
#include "decoder_buffer.h"
#include "decoder_buffer_queue.h"
#include "task_runner.h"

namespace media {

class Demuxer;

class DemuxerStream {

 public:

  DELETE_COPY_AND_ASSIGN(DemuxerStream);

  enum Type {
    UNKNOWN,
    Audio,
    Video,
    NUM_TYPES,  // Always keep this entry as the last one!
  };

  DemuxerStream(AVStream *stream,
                Demuxer *demuxer,
                Type type,
                std::unique_ptr<AudioDecodeConfig> audio_decode_config,
                std::unique_ptr<VideoDecodeConfig> video_decode_config);

  static std::shared_ptr<DemuxerStream> Create(media::Demuxer *demuxer, AVStream *stream);

  AVStream *stream() const {
    return stream_;
  }

  using ReadCallback = OnceCallback<void(std::shared_ptr<DecoderBuffer>)>;
  void Read(std::function<void(std::shared_ptr<DecoderBuffer>)> read_callback);

  void EnqueuePacket(std::unique_ptr<AVPacket, AVPacketDeleter> packet);

  Type type() {
    return type_;
  }

  AudioDecodeConfig audio_decode_config();

  VideoDecodeConfig video_decode_config();

  double duration();

  void SetEndOfStream() {
    end_of_stream_ = true;
  }

  // Returns the value associated with |key| in the metadata for the avstream.
  // Returns an empty string if the key is not present.
  std::string GetMetadata(const char *key) const;

  // Empties the queues and ignores any additional calls to Read().
  void Stop();

  bool HasAvailableCapacity();

  void SetEnabled(bool enabled, double timestamp);

 private:

  Demuxer *demuxer_;
  AVStream *stream_;
  std::unique_ptr<AudioDecodeConfig> audio_decode_config_;
  std::unique_ptr<VideoDecodeConfig> video_decode_config_;
  Type type_;

  std::shared_ptr<DecoderBufferQueue> buffer_queue_;

  TaskRunner *task_runner_;
  bool end_of_stream_;

  int64 last_packet_pos_;
  int64 last_packet_dts_;

  bool waiting_for_key_frame_;

  ReadCallback read_callback_;

  void SatisfyPendingRead();

};

} // namespace media

#endif //MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
