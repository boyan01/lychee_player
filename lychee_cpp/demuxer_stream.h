//
// Created by yangbin on 2021/4/19.
//

#ifndef MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
#define MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_

#include <ostream>

#include "base/basictypes.h"
#include "base/callback.h"
#include "base/task_runner.h"
#include "functional"

extern "C" {
#include "libavformat/avformat.h"
}

#include "audio_decode_config.h"
#include "decoder_buffer.h"
#include "decoder_buffer_queue.h"

namespace lychee {

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

  DemuxerStream(AVStream *stream, Demuxer *demuxer, Type type,
                std::unique_ptr<AudioDecodeConfig> audio_decode_config);

  static std::shared_ptr<DemuxerStream> Create(
      Demuxer *demuxer,
      AVStream *stream,
      AVFormatContext *format_context
  );

  [[nodiscard]] AVStream *stream() const { return stream_; }

  using ReadCallback = std::function<void(std::shared_ptr<DecoderBuffer>)>;
  void Read(ReadCallback read_callback);

  void EnqueuePacket(std::unique_ptr<AVPacket, AVPacketDeleter> packet);

  Type type() { return type_; }

  AudioDecodeConfig audio_decode_config();

  double duration();

  double GetBufferQueueDuration();

  void SetEndOfStream() { end_of_stream_ = true; }

  // Returns the value associated with |key| in the metadata for the avstream.
  // Returns an empty string if the key is not present.
  std::string GetMetadata(const char *key) const;

  // Empties the queues and ignores any additional calls to Read().
  void Stop();

  bool HasAvailableCapacity();

  void SetEnabled(bool enabled, double timestamp);

  void Abort();

  void FlushBuffers();

  friend std::ostream &operator<<(std::ostream &os,
                                  const DemuxerStream &stream);

  [[nodiscard]] int64 GetSerial() const { return serial_; }

 private:
  Demuxer *demuxer_;
  AVStream *stream_;
  std::unique_ptr<AudioDecodeConfig> audio_decode_config_;
  Type type_;

  std::shared_ptr<DecoderBufferQueue> buffer_queue_;

  media::TaskRunner task_runner_;
  bool end_of_stream_;

  int64 last_packet_pos_;
  int64 last_packet_dts_;

  bool waiting_for_key_frame_;

  ReadCallback read_callback_;

  int64 serial_;

  bool abort_;

  void ReadTask(ReadCallback read_callback);

  void SatisfyPendingRead();
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_DEMUXER_STREAM_H_
