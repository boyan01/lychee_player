//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_FFMPEG_DEMUXER_H_
#define MEDIA_PLAYER_FFMPEG_DEMUXER_H_

#include <memory>

extern "C" {
#include "libavformat/avformat.h"
};

#include "message_loop.h"

#include "decoder_buffer.h"
#include "data_source.h"

#include "audio_decoder_config.h"
#include "video_decoder_config.h"

namespace media {

class FFmpegDemuxer;

class FFmpegDemuxerStream {
 public:
  enum Type {
    UNKNOWN,
    AUDIO,
    VIDEO,
    NUM_TYPES,  // Always keep this entry as the last one!
  };

  FFmpegDemuxerStream(FFmpegDemuxer *demuxer, AVStream *stream);

  /**
   * @return The Type of stream.
   */
  Type type();

 protected:
 public:
  virtual ~FFmpegDemuxerStream();

 private:

  enum Status {
    kOk,
    kAborted,
    kConfigChanged,
  };

  // Request a buffer to returned via the provided callback.
  //
  // The first parameter indicates the status of the read.
  // The second parameter is non-NULL and contains media data
  // or the end of the stream if the first parameter is kOk. NULL otherwise.
  typedef std::function<void(Status,
                             const std::shared_ptr<DecoderBuffer> &)> ReadCB;

  void Read(const ReadCB &read_cb);

  // Convert an FFmpeg stream timestamp in to a chrono::microseconds.
  static chrono::microseconds ConvertStreamTimestamp(const AVRational &time_base, int64 timestamp);

  FFmpegDemuxer *demuxer_;
  AVStream *stream_;

  AudioDecoderConfig audio_config_;
  VideoDecoderConfig video_config_;

  Type type_;
  chrono::microseconds duration_;
  bool stopped_;

  typedef std::deque<std::shared_ptr<DecoderBuffer> > BufferQueue;
  BufferQueue buffer_queue_;

  typedef std::deque<ReadCB> ReadQueue;
  ReadQueue read_queue_;

  // mutex to synchronize access to `buffer_queue_`, `read_queue_`, and `stopped_`
  mutable std::mutex mutex_;

  DISALLOW_COPY_AND_ASSIGN(FFmpegDemuxerStream);

};

class FFmpegDemuxer {

 public:
  FFmpegDemuxer(std::shared_ptr<base::MessageLoop> message_loop, std::shared_ptr<DataSource> data_source);

 private:
  std::shared_ptr<base::MessageLoop> message_loop_;
  std::shared_ptr<DataSource> data_source_;

};

}

#endif //MEDIA_PLAYER_FFMPEG_DEMUXER_H_
