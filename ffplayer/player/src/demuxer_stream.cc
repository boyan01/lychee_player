//
// Created by yangbin on 2021/4/19.
//

#include "demuxer_stream.h"

#include "base/logging.h"
#include "base/bind_to_current_loop.h"

#include "demuxer.h"

namespace media {

std::shared_ptr<DemuxerStream> DemuxerStream::Create(media::Demuxer *demuxer, AVStream *stream) {
  Type type;
  std::unique_ptr<AudioDecodeConfig> audio_decode_config;
  std::unique_ptr<VideoDecodeConfig> video_decode_config;
  switch (stream->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
      type = Audio;
      audio_decode_config = std::make_unique<AudioDecodeConfig>(*stream->codecpar, stream->time_base);
      break;
    }
    case AVMEDIA_TYPE_VIDEO: {
      type = Video;
      video_decode_config = std::make_unique<VideoDecodeConfig>(*stream->codecpar, stream->time_base,
                                                                stream->avg_frame_rate,
                                                                10);
      break;
    }
    default:type = UNKNOWN;
      NOTREACHED();
      break;
  }

  return std::make_shared<DemuxerStream>(stream, demuxer, type,
                                         std::move(audio_decode_config),
                                         std::move(video_decode_config));
}

DemuxerStream::DemuxerStream(
    AVStream *stream,
    Demuxer *demuxer,
    Type type,
    std::unique_ptr<AudioDecodeConfig> audio_decode_config,
    std::unique_ptr<VideoDecodeConfig> video_decode_config
) : stream_(stream),
    demuxer_(demuxer),
    type_(type),
    audio_decode_config_(std::move(audio_decode_config)),
    video_decode_config_(std::move(video_decode_config)),
    task_runner_(TaskRunner::current()),
    buffer_queue_(std::make_shared<DecoderBufferQueue>()){

}

AudioDecodeConfig DemuxerStream::audio_decode_config() {
  DCHECK_EQ(type_, Audio);
  return *audio_decode_config_;
}

VideoDecodeConfig DemuxerStream::video_decode_config() {
  DCHECK_EQ(type_, Video);
  return *video_decode_config_;
}

void DemuxerStream::EnqueuePacket(std::unique_ptr<AVPacket, AVPacketDeleter> packet) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(packet->size);
  DCHECK(packet->data);

  DLOG(INFO) << __func__;

  const bool is_audio = type_ == Audio;

  auto packet_dts = packet->dts == AV_NOPTS_VALUE ? packet->pts : packet->dts;

  if (end_of_stream_) {
    NOTREACHED() << "Attempted to enqueue packet on a stooped stream";
    return;
  }

  last_packet_pos_ = packet->pos;
  last_packet_dts_ = packet_dts;

  if (waiting_for_key_frame_) {
    if (packet->flags & AV_PKT_FLAG_KEY) {
      waiting_for_key_frame_ = false;
    } else {
      DLOG(INFO) << "Dropped non-keyframe pts = " << packet->pts;
      return;
    }
  }

  auto pkt_pts = packet->pts;
  if (pkt_pts == AV_NOPTS_VALUE) {
    DLOG(WARNING) << "FFmpegDemuxer: PTS is not defined";
    return;
  }

  std::shared_ptr<DecoderBuffer> buffer = std::make_shared<DecoderBuffer>(std::move(packet));
  buffer->set_timestamp(av_q2d(stream_->time_base) * double(pkt_pts));

  buffer_queue_->Push(std::move(buffer));

}

void DemuxerStream::SatisfyPendingRead() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (read_callback_) {
    if (!buffer_queue_->IsEmpty()) {
      std::move(read_callback_)(buffer_queue_->Pop());
    } else if (end_of_stream_) {
      std::move(read_callback_)(DecoderBuffer::CreateEOSBuffer());
    }
  }

  if (!end_of_stream_ && HasAvailableCapacity()) {
    // TODO notify to read more.
    demuxer_->NotifyCapacityAvailable();
  }
}

bool DemuxerStream::HasAvailableCapacity() {
  // Try to have two second's worth of encoded data per stream.
  const double kCapacity = 2;
  return buffer_queue_->IsEmpty() || buffer_queue_->Duration() < kCapacity;
}

void DemuxerStream::Read(std::function<void(std::shared_ptr<DecoderBuffer>)> read_callback) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(!read_callback_) << "Overlapping reads are not supported.";
  read_callback_ = ReadCallback(std::move(BindToCurrentLoop(std::move(read_callback))));

  if (!stream_) {
    std::move(read_callback_)(DecoderBuffer::CreateEOSBuffer());
    return;
  }
  SatisfyPendingRead();
}

double DemuxerStream::duration() {
  return av_q2d(stream_->time_base) * double(stream_->duration);
}

std::string DemuxerStream::GetMetadata(const char *key) const {
  const AVDictionaryEntry *entry =
      av_dict_get(stream_->metadata, key, nullptr, 0);
  return (entry == nullptr || entry->value == nullptr) ? "" : entry->value;
}

void DemuxerStream::Stop() {
  DCHECK(task_runner_->BelongsToCurrentThread());

  buffer_queue_->Clear();
  stream_ = nullptr;
  demuxer_ = nullptr;
  end_of_stream_ = true;

  if (read_callback_) {
    std::move(read_callback_)(DecoderBuffer::CreateEOSBuffer());
  }

}

} // namespace media