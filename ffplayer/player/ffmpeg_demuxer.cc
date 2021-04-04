//
// Created by yangbin on 2021/3/28.
//

#include <utility>

#include "base/logging.h"

#include "ffmpeg_common.h"
#include "ffmpeg_demuxer.h"
#include "ffmpeg_glue.h"

namespace media {

DemuxerHost::~DemuxerHost() = default;

FFmpegDemuxerStream::FFmpegDemuxerStream(FFmpegDemuxer *demuxer, AVStream *stream)
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

bool FFmpegDemuxerStream::HasPendingReads() {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());
  std::lock_guard<std::mutex> auto_lock(mutex_);
  DCHECK(!stopped_ || read_queue_.empty())
  << "Read queue should have been emptied if demuxing stream is stopped";
  return !read_queue_.empty();
}

void FFmpegDemuxerStream::EnqueuePacket(AVPacketUniquePtr packet) {
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

void FFmpegDemuxerStream::Read(const FFmpegDemuxerStream::ReadCB &read_cb) {
  DCHECK(!read_cb);

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

void FFmpegDemuxerStream::Stop() {
  DCHECK(demuxer_->message_loop()->BelongsToCurrentThread());
  std::lock_guard<std::mutex> auto_lock(mutex_);
  buffer_queue_.clear();
  for (auto &read_cb : read_queue_) {
    read_cb(kOk, DecoderBuffer::CreateEOSBuffer());
  }
  read_queue_.clear();
  stopped_ = true;
}

chrono::microseconds FFmpegDemuxerStream::duration() {
  return duration_;
}

FFmpegDemuxerStream::Type FFmpegDemuxerStream::type() {
  return type_;
}

TimeDelta FFmpegDemuxerStream::GetElapsedTime() const {
  return ConvertStreamTimestamp(stream_->time_base, stream_->cur_dts);
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

Ranges<TimeDelta> FFmpegDemuxerStream::GetBufferedRanges() const {
  std::lock_guard<std::mutex> auto_lock(mutex_);
  return buffered_ranges_;
}

FFmpegDemuxer::FFmpegDemuxer(
    std::shared_ptr<base::MessageLoop> message_loop,
    std::shared_ptr<DataSource> data_source)
    : message_loop_(std::move(message_loop)),
      data_source_(std::move(data_source)),
      host_(nullptr),
      format_context_(nullptr),
      bitrate_(0),
      start_time_(kNoTimestamp()),
      audio_disabled_(false),
      duration_known_(false) {
  DCHECK(message_loop_);
  DCHECK(data_source_);
}

void FFmpegDemuxer::PostDemuxTask() {
  message_loop_->PostTask(FROM_HERE, [this]() {
    DemuxTask();
  });
}

void FFmpegDemuxer::DemuxTask() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  // Make sure we have work to do before demuxing.
  if (!StreamsHavePendingReads()) {
    return;
  }

  // Allocate and read an AVPacket from the media.
  AVPacketUniquePtr packet(new AVPacket(), UniquePtrAVFreePacket());
  int result = av_read_frame(format_context_, packet.get());
  if (result < 0) {
    // Update the duration based on the audio stream if it was previously unknown.
    // http://crbug.com/86830
    if (!duration_known_) {
      // Search streams for AUDIO one.
      for (auto &stream : streams_) {
        if (stream && stream->type() == FFmpegDemuxerStream::AUDIO) {
          auto duration = stream->GetElapsedTime();
          if (duration != kNoTimestamp() && duration > TimeDelta()) {
            host_->SetDuration(duration);
            duration_known_ = true;
          }
          break;
        }
      }
    }

    // If we have reached the end of stream, tell the downstream filters about the event.
    StreamHasEnded();
    return;
  }

  // Queue the packet with the appropriate streams.
  DCHECK_GE(packet->stream_index, 0);
  DCHECK_LT(packet->stream_index, static_cast<int>(streams_.size()));

  if (packet->stream_index >= 0 && packet->stream_index < streams_.size() && streams_[packet->stream_index]
      && (!audio_disabled_ || streams_[packet->stream_index]->type() != FFmpegDemuxerStream::AUDIO)) {
    auto demuxer_stream = streams_[packet->stream_index];
    demuxer_stream->EnqueuePacket(std::move(packet));
  }

  // Create a loop by posting another task.  This allows seek and message loop
  // quit tasks to get processed.
  if (StreamsHavePendingReads()) {
    PostDemuxTask();
  }

}

bool FFmpegDemuxer::StreamsHavePendingReads() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  return std::any_of(streams_.begin(), streams_.end(), [](const std::shared_ptr<FFmpegDemuxerStream> &stream) -> bool {
    return stream->HasPendingReads();
  });
}

void FFmpegDemuxer::Initialize(DemuxerHost *host, const PipelineStatusCB &status_cb) {
  message_loop_->PostTask(FROM_HERE, [this, host, status_cb]() {
    InitializeTask(host, status_cb);
  });
}

// Helper for calculating the bitrate of the media based on information stored
// in |format_context| or failing that the size and duration of the media.
//
// Returns 0 if a bitrate could not be determined.
static int CalculateBitrate(
    AVFormatContext *format_context,
    const TimeDelta &duration,
    int64 filesize_in_bytes) {
  // If there is a bitrate set on the container, use it.
  if (format_context->bit_rate > 0)
    return format_context->bit_rate;

  // Then try to sum the bitrates individually per stream.
  int bitrate = 0;
  for (size_t i = 0; i < format_context->nb_streams; ++i) {
    auto *codec_par = format_context->streams[i]->codecpar;
    bitrate += codec_par->bit_rate;
  }
  if (bitrate > 0)
    return bitrate;

  // See if we can approximate the bitrate as long as we have a filesize and
  // valid duration.
  if (duration.count() <= 0 ||
      duration == kInfiniteDuration() ||
      filesize_in_bytes == 0) {
    return 0;
  }

  // Do math in floating point as we'd overflow an int64 if the filesize was
  // larger than ~1073GB.
  double bytes = filesize_in_bytes;
  double duration_us = duration.count();
  return int(bytes * 8000000.0 / duration_us);
}

void FFmpegDemuxer::InitializeTask(DemuxerHost *host, const PipelineStatusCB &status_cb) {
  DCHECK(message_loop_->BelongsToCurrentThread());

  host_ = host;

  data_source_->SetHost(host);

  // Add ourself to Protocol list and get our unique key.
  std::string key = FFmpegGlue::GetInstance()->AddProtocol(this);

  // Open FFmpeg AVFormatContext.
  DCHECK(!format_context_);
  AVFormatContext *context = avformat_alloc_context();

  // Disable ID3v1 tag reading to avoid costly seeks to end of file for data we
  // don't use.  FFmpeg will only read ID3v1 tags if no other metadata is
  // available, so add a metadata entry to ensure some is always present.
  av_dict_set(&context->metadata, "skip_id3v1_tags", "", 0);

  int result = avformat_open_input(&context, key.c_str(), nullptr, nullptr);

  // Remove ourself from protocol list.
  FFmpegGlue::GetInstance()->RemoveProtocol(this);

  if (result < 0) {
    status_cb(PipelineStatus::DEMUXER_ERROR_COULD_NOT_OPEN);
    return;
  }

  DCHECK(context);
  format_context_ = context;

  // Fully initialize AVFormatContext by parsing the stream a little.
  result = avformat_find_stream_info(format_context_, nullptr);
  if (result < 0) {
    status_cb(PipelineStatus::DEMUXER_ERROR_COULD_NOT_PARSE);
    return;
  }

  // Create demuxer stream entries for each possible AVStreams.
  streams_.resize(format_context_->nb_streams);
  bool found_audio_stream = false;
  bool found_video_stream = false;

  TimeDelta max_duration;

  for (size_t i = 0; i < format_context_->nb_streams; i++) {
    auto *codec_par = format_context_->streams[i]->codecpar;
    auto codec_type = codec_par->codec_type;

    if (codec_type == AVMEDIA_TYPE_AUDIO) {
      if (found_audio_stream)
        continue;
      // Ensure the codec is supported.
      if (CodecIDToAudioCodec(codec_par->codec_id) == kUnknownAudioCodec)
        continue;
      found_audio_stream = true;
    } else if (codec_type == AVMEDIA_TYPE_VIDEO) {
      if (found_video_stream)
        continue;
      // Ensure the codec iis supported.
      if (CodecIDToVideoCodec(codec_par->codec_id) == kUnknownVideoCodec)
        continue;
      found_video_stream = true;
    } else {
      continue;
    }

    AVStream *stream = format_context_->streams[i];
    auto demuxer_stream = std::make_shared<FFmpegDemuxerStream>(this, stream);

    streams_[i] = demuxer_stream;
    max_duration = std::max(max_duration, demuxer_stream->duration());

    if (stream->first_dts != static_cast<int64_t>(AV_NOPTS_VALUE)) {
      const auto first_dts = ConvertFromTimeBase(stream->time_base, stream->first_dts);
      if (start_time_ == kNoTimestamp() || first_dts < start_time_) {
        start_time_ = first_dts;
      }
    }
  }

  if (!found_audio_stream && !found_video_stream) {
    status_cb(PipelineStatus::DEMUXER_ERROR_NO_SUPPORTED_STREAMS);
    return;
  }

  if (format_context_->duration != AV_NOPTS_VALUE) {
    const AVRational av_time_base = {1, AV_TIME_BASE};
    max_duration = std::max(max_duration, ConvertFromTimeBase(av_time_base, format_context_->duration));
  } else {
    max_duration = kInfiniteDuration();
  }

  // Some demuxer, like WAV, do not put timestamps on their frames, We assume the start time is 0.
  if (start_time_ == kNoTimestamp()) {
    start_time_ = TimeDelta();
  }

  // Set the duration and bitrate and notify we're done initializing.
  host->SetDuration(max_duration);
  duration_known_ = (max_duration != kInfiniteDuration());

  int64 filesize_in_types = 0;
  GetSize(&filesize_in_types);
  bitrate_ = CalculateBitrate(format_context_, max_duration, filesize_in_types);
  if (bitrate_ > 0) {
    data_source_->SetBitrate(bitrate_);
  }

  status_cb(PipelineStatus::OK);
}

void FFmpegDemuxer::StreamHasEnded() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  for (auto &stream: streams_) {
    if (audio_disabled_ && stream->type() == FFmpegDemuxerStream::AUDIO) {
      continue;
    }
    // FIXME: flush packet.
    AVPacketUniquePtr packet(new AVPacket(), UniquePtrAVFreePacket());
    stream->EnqueuePacket(std::move(packet));
  }
}

void FFmpegDemuxer::NotifyBufferingChanged() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  Ranges<TimeDelta> buffered;
  std::shared_ptr<FFmpegDemuxerStream> audio = audio_disabled_ ? nullptr : GetFFmpegStream(FFmpegDemuxerStream::AUDIO);
  std::shared_ptr<FFmpegDemuxerStream> video = GetFFmpegStream(FFmpegDemuxerStream::VIDEO);
  if (audio && video) {
    buffered = audio->GetBufferedRanges().IntersectionWith(video->GetBufferedRanges());
  } else if (audio_disabled_) {
    buffered = audio->GetBufferedRanges();
  } else if (video) {
    buffered = video->GetBufferedRanges();
  }
  for (size_t i = 0; i < buffered.size(); ++i) {
    host_->AddBufferedTimeRange(buffered.start(i), buffered.end(i));
  }
}

std::shared_ptr<FFmpegDemuxerStream> FFmpegDemuxer::GetFFmpegStream(FFmpegDemuxerStream::Type type) const {
  for (auto &stream : streams_) {
    if (stream->type() == type) {
      return stream;
    }
  }
  return nullptr;
}

void FFmpegDemuxer::Stop(const std::function<void(void)> &callback) {
  message_loop_->PostTask(FROM_HERE, [&]() {
    StopTask(callback);
  });

  // Then wakes up the thread from reading.
  SignalReadCompleted(DataSource::kReadError);
}

void FFmpegDemuxer::StopTask(const std::function<void(void)> &callback) {
  DCHECK(message_loop_->BelongsToCurrentThread());
  for (auto &stream: streams_) {
    stream->Stop();
  }
  if (data_source_) {
    data_source_->Stop(callback);
  } else {
    callback();
  }
}

void FFmpegDemuxer::SignalReadCompleted(int size) {
  last_read_bytes_ = size;
//  read_event_.Signal();
}

}
