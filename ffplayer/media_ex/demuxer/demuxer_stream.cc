//
// Created by yangbin on 2021/4/5.
//
#include "demuxer/demuxer_stream.h"

#include "base/logging.h"

#include "ffmpeg/ffmpeg_utils.h"
#include "demuxer/demuxer.h"

namespace media {

DemuxerStream::DemuxerStream(Demuxer *demuxer, AVStream *stream)
    : demuxer_(demuxer),
      stream_(stream),
      type_(Type::UNKNOWN),
      stopped_(false),
      duration_(chrono::microseconds::zero()),
      last_packet_timestamp_(kNoTimestamp()) {
  DCHECK(demuxer_);

  switch (stream->codecpar->codec_type) {
    case AVMEDIA_TYPE_AUDIO: {
      type_ = AUDIO;
      ffmpeg::AVCodecContextToAudioDecoderConfig(stream->codec, &audio_config_);
      break;
    }
    case AVMEDIA_TYPE_VIDEO: {
      type_ = VIDEO;
      ffmpeg::AVStreamToVideoDecoderConfig(stream, &video_config_);
      break;
    }
    default: {
      NOTREACHED();
      break;
    }
  }

  duration_ = ConvertStreamTimestamp(stream->time_base, stream->duration);
}

DemuxerStream::~DemuxerStream() {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  DCHECK(stopped_);
  DCHECK(read_queue_.empty());
  DCHECK(buffer_queue_.empty());
}

bool DemuxerStream::HasPendingReads() {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());
  std::lock_guard<std::mutex> auto_lock(mutex_);
  DCHECK(!stopped_ || read_queue_.empty())
  << "Read queue should have been emptied if demuxing stream is stopped";
  return !read_queue_.empty();
}

void DemuxerStream::EnqueuePacket(unique_ptr_d<AVPacket> packet) {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());

  std::lock_guard<std::mutex> auto_lock(mutex_);
  if (stopped_) {
    NOTREACHED() << "Attempted to enqueue packet to a stopped stream";
    return;
  }

  std::shared_ptr<DecoderBuffer> buffer;
  if (!packet.get()) {
    buffer = DecoderBuffer::CreateEOSBuffer();
  }

  // If a packet is returned by FFmpeg's av_parser_parse2() the packet will
  // reference inner memory of FFmpeg.  As such we should transfer the packet
  // into memory we control.
  buffer = DecoderBuffer::CopyFrom(packet->data, packet->size);
  buffer->SetTimeStamp(ConvertStreamTimestamp(stream_->time_base, packet->pts));
  buffer->SetDuration(ConvertStreamTimestamp(stream_->time_base, packet->duration));
  if (buffer->GetTimestamp() != kNoTimestamp() && last_packet_timestamp_ != kNoTimestamp() &&
      last_packet_timestamp_ < buffer->GetTimestamp()) {
    buffered_ranges_.Add(last_packet_timestamp_, buffer->GetTimestamp());
    demuxer_->message_loop()->PostTask(FROM_HERE, [&]() {
      demuxer_->NotifyBufferingChanged();
    });
    last_packet_timestamp_ = buffer->GetTimestamp();
  }

  buffer_queue_.push_back(buffer);
  FulfillPendingRead();
}

void DemuxerStream::Read(const DemuxerStream::ReadCB &read_cb) {
  DCHECK(!read_cb);

  std::lock_guard<std::mutex> auto_lock(mutex_);

  // Don't accept any additional reads if we've been told to stop.
  // The demuxer may have been destroyed.
  if (stopped_) {
    read_cb(DemuxerStream::kOk, DecoderBuffer::CreateEOSBuffer());
    return;
  }

  // Buffers are only queued when there are no pending reads.
  DCHECK(buffer_queue_.empty() || read_queue_.empty());

  if (buffer_queue_.empty()) {
    demuxer_->message_loop()->PostTask(FROM_HERE, [this, read_cb] {
      ReadTask(read_cb);
    });
    return;;
  }

  // Send the oldest buffer back.
  auto buffer = buffer_queue_.front();
  buffer_queue_.pop_front();
  read_cb(DemuxerStream::kOk, buffer);
}

void DemuxerStream::Stop() {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());
  std::lock_guard<std::mutex> auto_lock(mutex_);
  buffer_queue_.clear();
  for (auto &read_cb : read_queue_) {
    read_cb(kOk, DecoderBuffer::CreateEOSBuffer());
  }
  read_queue_.clear();
  stopped_ = true;
}

chrono::microseconds DemuxerStream::duration() {
  return duration_;
}

DemuxerStream::Type DemuxerStream::type() {
  return type_;
}

TimeDelta DemuxerStream::GetElapsedTime() const {
  return ConvertStreamTimestamp(stream_->time_base, stream_->cur_dts);
}

void DemuxerStream::ReadTask(const DemuxerStream::ReadCB &read_cb) {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());

  std::lock_guard<std::mutex> auto_lock(mutex_);

  // Don't accept any additional reads if we've been told to stop.
  // The demuxer may have been destroyed.
  if (stopped_) {
    read_cb(DemuxerStream::kOk, DecoderBuffer::CreateEOSBuffer());
    return;
  }

  // Enqueue the callback and attempt to satisfy it immediately.
  read_queue_.push_back(read_cb);
  FulfillPendingRead();

  // Check if there are still pending reads, calling demuxer for help.
  if (!read_queue_.empty()) {
    demuxer_->PostDemuxTask();
  }

}

void DemuxerStream::FulfillPendingRead() {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());

  if (buffer_queue_.empty() || read_queue_.empty()) {
    return;
  }

  // Dequeue a buffer and pending read pair.
  auto buffer = buffer_queue_.front();
  ReadCB read_cb(read_queue_.front());
  buffer_queue_.pop_front();
  read_queue_.pop_front();

  read_cb(DemuxerStream::kOk, buffer);
}

// static
chrono::microseconds DemuxerStream::ConvertStreamTimestamp(const AVRational &time_base, int64 timestamp) {
  if (timestamp == static_cast<int64>(AV_NOPTS_VALUE)) {
    return chrono::microseconds::min();
  }
  return ffmpeg::ConvertFromTimeBase(time_base, timestamp);
}

Ranges<TimeDelta> DemuxerStream::GetBufferedRanges() const {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  return buffered_ranges_;
}

}