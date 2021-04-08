//
// Created by yangbin on 2021/4/5.
//

#ifndef MEDIA_PLAYER_DEMUXER_DEMUXER_STREAM_H_
#define MEDIA_PLAYER_DEMUXER_DEMUXER_STREAM_H_

#include "mutex"
#include "memory"
#include "queue"
#include "string"

#include "base/timestamps.h"
#include "base/basictypes.h"
#include "base/ranges.h"

#include "decoder/decoder_buffer.h"
#include "decoder/video_decoder_config.h"
#include "decoder/audio_decoder_config.h"

extern "C" {
#include "libavformat/avformat.h"
}

namespace media {

struct Demuxer;

class DemuxerStream {
 public:
  enum Type {
    UNKNOWN,
    AUDIO,
    VIDEO,
    NUM_TYPES,  // Always keep this entry as the last one!
  };

  static std::unique_ptr<DemuxerStream> Create(Demuxer *demuxer, AVStream *stream) {
    return std::make_unique<DemuxerStream>(demuxer, stream);
  }

  DemuxerStream(Demuxer *demuxer, AVStream *stream);

  // Returns true is this stream has pending reads, false otherwise.
  //
  // Safe to call on any thread.
  bool HasPendingReads();

  // Enqueues the given AVPacket.  If |packet| is NULL an end of stream packet
  // is enqueued.
  void EnqueuePacket(unique_ptr_d<AVPacket> packet);

  /**
   * @return The Type of stream.
   */
  Type type();

  /**
   * Returns the duration of this stream.
   */
  TimeDelta duration();

  /**
   * Returns elapsed time based on the already queued packets.
   * Used to determine stream duration when it's not known ahead of time.
   */
  TimeDelta GetElapsedTime() const;

  // Returns the range of buffered data in this stream.
  Ranges<TimeDelta> GetBufferedRanges() const;

  // Empties the queues and ignores any additional calls to Read().
  void Stop();

  // Returns the value associated with |key| in the metadata for the avstream.
  // Returns an empty string if the key is not present.
  std::string GetMetadata(const char *key) const {
    const AVDictionaryEntry *entry =
        av_dict_get(stream_->metadata, key, nullptr, 0);
    return (entry == nullptr || entry->value == nullptr) ? "" : entry->value;
  }

  void SetEndOfStream() {
//    DCHECK(task_runner_->RunsTasksInCurrentSequence());
//    end_of_stream_ = true;
//    SatisfyPendingRead();
  }

  AudioDecoderConfig audio_decoder_config() {
    return audio_config_;
  }

  VideoDecoderConfig video_decoder_config() {
    return video_config_;
  }

 protected:
 public:
  virtual ~DemuxerStream();

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
  typedef std::function<void(Status, const std::shared_ptr<DecoderBuffer> &)> ReadCB;

  void Read(const ReadCB &read_cb);

  // Carries out enqueuing a pending read on the demuxer thread.
  void ReadTask(const ReadCB &read_cb);

  // Attempts to fulfill a single pending read by dequeueing a buffer and read
  // callback pair and executing the callback. The calling function must
  // acquire `lock_` before calling this function.
  void FulfillPendingRead();

  // Convert an FFmpeg stream timestamp in to a TimeDelta.
  static TimeDelta ConvertStreamTimestamp(const AVRational &time_base, int64 timestamp);

  Demuxer *demuxer_;
  AVStream *stream_;

  AudioDecoderConfig audio_config_;
  VideoDecoderConfig video_config_;

  Type type_;
  chrono::microseconds duration_;
  bool stopped_;

  TimeDelta last_packet_timestamp_;
  Ranges<TimeDelta> buffered_ranges_;

  typedef std::deque<std::shared_ptr<DecoderBuffer>> BufferQueue;
  BufferQueue buffer_queue_;

  typedef std::deque<ReadCB> ReadQueue;
  ReadQueue read_queue_;

  // mutex to synchronize access to `buffer_queue_`, `read_queue_`, and `stopped_`
  mutable std::mutex mutex_;

  DISALLOW_COPY_AND_ASSIGN(DemuxerStream);

};

} // namespace media

#endif //MEDIA_PLAYER_DEMUXER_DEMUXER_STREAM_H_
