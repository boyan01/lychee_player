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

void DemuxerStream::EnqueuePacket(std::unique_ptr<AVPacket, AVPacketDeleter> packet) {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(packet->size);
  DCHECK(packet->data);

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
    continue_read_thread_->notify_all();
  }
}

bool DemuxerStream::HasAvailableCapacity() {
  // Try to have two second's worth of encoded data per stream.
  const double kCapacity = 2;
  return buffer_queue_->IsEmpty() || buffer_queue_->Duration() < kCapacity;
}

} // namespace media