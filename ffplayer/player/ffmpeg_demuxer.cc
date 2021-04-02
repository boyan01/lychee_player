//
// Created by yangbin on 2021/3/28.
//

#include <utility>

#include "logging.h"

#include "ffmpeg_common.h"
#include "ffmpeg_demuxer.h"

namespace media {

DemuxerHost::~DemuxerHost() = default;

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
  DCHECK_NE(read_cb, nullptr);

  std::lock_guard<std::mutex> auto_lock(mutex_);

  // Don't accept any additional reads if we've been told to stop.
  // The demuxer may have been destroyed.
  if (stopped_) {
    read_cb(FFmpegDemuxerStream::kOk, DecoderBuffer::CreateEOSBuffer());
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
  read_cb(FFmpegDemuxerStream::kOk, buffer);
}

FFmpegDemuxerStream::Type FFmpegDemuxerStream::type() {
  return type_;
}

void FFmpegDemuxerStream::ReadTask(const FFmpegDemuxerStream::ReadCB &read_cb) {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());

  std::lock_guard<std::mutex> auto_lock(mutex_);

  // Don't accept any additional reads if we've been told to stop.
  // The demuxer may have been destroyed.
  if (stopped_) {
    read_cb(FFmpegDemuxerStream::kOk, DecoderBuffer::CreateEOSBuffer());
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

void FFmpegDemuxerStream::FulfillPendingRead() {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());

  if (buffer_queue_.empty() || read_queue_.empty()) {
    return;
  }

  // Dequeue a buffer and pending read pair.
  auto buffer = buffer_queue_.front();
  ReadCB read_cb(read_queue_.front());
  buffer_queue_.pop_front();
  read_queue_.pop_front();

  read_cb(FFmpegDemuxerStream::kOk, buffer);
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

void FFmpegDemuxer::PostDemuxTask() {
  message_loop_->PostTask(FROM_HERE, [this]() {
    DemuxTask();
  });
}

void FFmpegDemuxer::DemuxTask() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  // Make sure we have work to do before demuxing.
  if (!StreamHavePendingReads()) {
    return;
  }

  // Allocate and read an AVPacket from the media.
  AVPacketUniquePtr packet(new AVPacket(), UniquePtrAVFreePacket());
  int result = av_read_frame(format_context_, packet.get());
  if (result < 0) {
    // Update the duration based on the audio stream if it was previously unknown.
    // http://crbug.com/86830
    if (!duration_known_) {
      // Search
    }
  }

}

bool FFmpegDemuxer::StreamHavePendingReads() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  return false;
}

void FFmpegDemuxer::Initialize(DemuxerHost *host, const PipelineStatusCB &status_cb) {
  message_loop_->PostTask(FROM_HERE, [this, host, status_cb]() {
    InitializeTask(host, status_cb);
  });
}

void FFmpegDemuxer::InitializeTask(DemuxerHost *host, const PipelineStatusCB &status_cb) {
  DCHECK(message_loop_->BelongsToCurrentThread());

  host_ = host;

  data_source_->SetHost(host);

  std::string key = "";

  // Open FFmpeg AVFormatContext.
  DCHECK(!format_context_);
  AVFormatContext *context = avformat_alloc_context();

  // Disable ID3v1 tag reading to avoid costly seeks to end of file for data we
  // don't use.  FFmpeg will only read ID3v1 tags if no other metadata is
  // available, so add a metadata entry to ensure some is always present.
  av_dict_set(&context->metadata, "skip_id3v1_tags", "", 0);

  int result = avformat_open_input(&context, key.c_str(), nullptr, nullptr);


}

}
