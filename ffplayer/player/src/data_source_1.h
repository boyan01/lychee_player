//
// Created by boyan on 2021/2/10.
//

#ifndef FFPLAYER_FFP_DATA_SOURCE_H
#define FFPLAYER_FFP_DATA_SOURCE_H

#include <thread>
#include <condition_variable>
#include <functional>

#include "ffp_packet_queue.h"
#include "media_clock.h"
#include "ffplayer.h"
#include "ffp_msg_queue.h"
#include "audio_decode_config.h"
#include "demuxer_stream.h"

extern "C" {
#include "libavformat/avformat.h"
};

namespace media {

class DataSource1 {
 public:
  bool gen_pts = false;
  bool find_stream_info = true;
  int seek_by_bytes = -1;

  int64_t start_time = AV_NOPTS_VALUE;

  char *wanted_stream_spec[AVMEDIA_TYPE_NB];

  PlayerConfiguration configuration;

  std::shared_ptr<PacketQueue> audio_queue;
  std::shared_ptr<PacketQueue> video_queue;
  std::shared_ptr<PacketQueue> subtitle_queue;

  std::shared_ptr<MessageContext> msg_ctx;

  Clock *ext_clock;

  std::function<void()> on_new_packet_send_;

  int read_pause_return;

  bool paused = false;

  void Seek(double position);

  double GetDuration();

  int GetChapterCount();

  int GetChapterByPosition(int64_t position);

  void SeekToChapter(int chapter);

  const char *GetMetadataDict(const char *key);

 private:
  char *filename;
  AVInputFormat *in_format;
  std::shared_ptr<std::condition_variable_any> continue_read_thread_;
  std::thread *read_tid = nullptr;
  bool abort_request = false;
  AVFormatContext *format_ctx_ = nullptr;
  bool realtime_ = false;

  int audio_stream_index = -1;
  int video_stream_index = -1;
  int subtitle_stream_index = -1;

  // request for seek.
  bool seek_req_ = false;
  int64_t seek_position = 0;

  // request for attached_pic.
  bool queue_attachments_req_ = false;

  bool eof = false;

  AVStream *video_stream_ = nullptr;
  AVStream *audio_stream_ = nullptr;
  AVStream *subtitle_stream_ = nullptr;

  int64_t duration = AV_NOPTS_VALUE;

  // buffer infinite.
  bool infinite_buffer = false;

 public:

  DataSource1(const char *filename, AVInputFormat *format);

  ~DataSource1();

  using OpenCallback = std::function<void(int)>;
  void Open(OpenCallback open_callback);

  bool ContainVideoStream();

  bool ContainAudioStream();

  DemuxerStream *video_demuxer_stream() {
    return video_demuxer_stream_.get();
  }

  VideoDecodeConfig video_decode_config() {
    return video_decode_config_;
  }

  DemuxerStream *audio_demuxer_stream() {
    return audio_demuxer_stream_.get();
  }

  const AudioDecodeConfig &audio_decode_config() {
    return audio_decode_config_;
  }

  bool ContainSubtitleStream();

  double GetSeekPosition() const;

  const char *GetFileName() const;

  bool IsReadComplete() const;

 private:

  std::shared_ptr<DemuxerStream> video_demuxer_stream_;
  std::shared_ptr<DemuxerStream> audio_demuxer_stream_;

  VideoDecodeConfig video_decode_config_;
  AudioDecodeConfig audio_decode_config_;

  OpenCallback open_callback_;

  int PrepareFormatContext();

  void OnFormatContextOpen();

  int ReadStreamInfo(int st_index[AVMEDIA_TYPE_NB]);

  void ReadThread();

  void OnStreamInfoLoad(const int st_index[AVMEDIA_TYPE_NB]);

  void InitVideoDecoder(int stream_index);

  void InitAudioDecoder(int stream_index);

  int OpenStreams(const int st_index[AVMEDIA_TYPE_NB]);

  void ReadStreams(std::mutex &read_mutex);

  void ProcessSeekRequest();

  void ProcessAttachedPicture();

  bool isNeedReadMore();

  int ProcessReadFrame(AVPacket *pkt, std::mutex &read_mutex);

  void ProcessQueuePacket(AVPacket *pkt);

};

}

#endif //FFPLAYER_FFP_DATA_SOURCE_H
