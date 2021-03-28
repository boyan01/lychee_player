//
// Created by yangbin on 2021/3/28.
//

#include <utility>

#include "logging.h"

#include "ffmpeg_common.h"
#include "ffmpeg_demuxer.h"

namespace media {

FFmpegDemuxerStream::FFmpegDemuxerStream(FFmpegDemuxer *demuxer, AVStream *stream)
    : demuxer_(demuxer),
      stream_(stream),
      type_(Type::UNKNOWN),
      stopped_(false),
      duration_(chrono::microseconds::zero()) {
  DCHECK(demuxer_);

  switch (stream->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
      type_ = AUDIO;
      AVCodecContextToAudioDecoderConfig(stream->codec, &audio_config_);
      break;
    }
    case AVMEDIA_TYPE_VIDEO: {
      type_ = VIDEO;
      AVStreamToVideoDecoderConfig(stream, &video_config_);
      break;
    }
    default: {
      NOTREACHED();
      break;
    }
  }

  duration_ = ConvertStreamTimestamp(stream->time_base, stream->duration);
}

FFmpegDemuxerStream::~FFmpegDemuxerStream() {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  DCHECK(stopped_);
  DCHECK(read_queue_.empty());
  DCHECK(buffer_queue_.empty());
}

void FFmpegDemuxerStream::Read(const FFmpegDemuxerStream::ReadCB &read_cb) {

}

// static
chrono::microseconds FFmpegDemuxerStream::ConvertStreamTimestamp(const AVRational &time_base, int64 timestamp) {
  if (timestamp == static_cast<int64>(AV_NOPTS_VALUE)) {
    return chrono::microseconds::min();
  }
  return ConvertFromTimeBase(time_base, timestamp);
}

FFmpegDemuxer::FFmpegDemuxer(
    std::shared_ptr<base::MessageLoop> message_loop,
    std::shared_ptr<DataSource> data_source)
    : message_loop_(std::move(message_loop)),
      data_source_(std::move(data_source)) {

}

}
