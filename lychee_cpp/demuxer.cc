//
// Created by boyan on 22-5-3.
//

#include "demuxer.h"

#include <utility>

#include "ffmpeg_utils.h"

namespace lychee {

namespace {

double ExtractStartTime(AVStream *stream) {
  // The default start time is zero.
  double start_time;

  // First try to use  the |start_time| value as is.
  if (stream->start_time != AV_NOPTS_VALUE)
    start_time =
        ffmpeg::ConvertFromTimeBase(stream->time_base, stream->start_time);

  // Next try to use the first DTS value, for codecs where we know PTS == DTS
  // (excludes all H26x codecs). The start time must be returned in PTS.
  if (stream->first_dts != AV_NOPTS_VALUE &&
      stream->codecpar->codec_id != AV_CODEC_ID_HEVC &&
      stream->codecpar->codec_id != AV_CODEC_ID_H264 &&
      stream->codecpar->codec_id != AV_CODEC_ID_MPEG4) {
    const double first_pts =
        ffmpeg::ConvertFromTimeBase(stream->time_base, stream->first_dts);
    if (first_pts < start_time) start_time = first_pts;
  }

  return start_time;
}
}

Demuxer::Demuxer(
    const media::TaskRunner &task_runner,
    std::string url,
    DemuxerHost *demuxer_host
) : task_runner_(task_runner),
    url_(std::move(url)),
    format_context_(nullptr),
    abort_request_(false),
    audio_stream_(),
    host_(demuxer_host) {

}

void Demuxer::Initialize(InitializeCallback callback) {
  initialize_callback_ = std::move(callback);
  task_runner_.PostTask(FROM_HERE, [&]() {
    format_context_ = avformat_alloc_context();
    format_context_->interrupt_callback.opaque = this;
    format_context_->interrupt_callback.callback = [](void *opaque) -> int {
      auto demuxer = static_cast<Demuxer *>(opaque);
      return demuxer->abort_request_;
    };

    auto ret = avformat_open_input(&format_context_, url_.c_str(), nullptr, nullptr);
    DLOG_IF(ERROR, ret < 0) << "failed to open avformat context." << ffmpeg::AVErrorToString(ret);

    task_runner_.PostTask(FROM_HERE_WITH_EXPLICIT_FUNCTION("Demuxer::Initialize"), [this]() {
      OnInitializeDone();
    });
  });
}

void Demuxer::OnInitializeDone() {
  DCHECK(task_runner_.BelongsToCurrentThread());
  if (!format_context_) {
    return;
  }

  av_format_inject_global_side_data(format_context_);
  auto result = avformat_find_stream_info(format_context_, nullptr);
  if (result < 0) {
    DLOG(ERROR) << "find stream info failed";
  }

  for (size_t i = 0; i < format_context_->nb_streams; ++i) {
    auto stream = format_context_->streams[i];

    const auto codec_parameters = stream->codecpar;

    if (codec_parameters->codec_type == AVMEDIA_TYPE_AUDIO) {
      stream->discard = AVDISCARD_DEFAULT;
      audio_stream_ = DemuxerStream::Create(this, stream, format_context_);
      break;
    } else {
      stream->discard = AVDISCARD_ALL;
    }
  }

  if (!audio_stream_) {
    DLOG(ERROR) << "failed to find audio stream";
    return;
  }

  // load stream complete.
  if (initialize_callback_) {
    initialize_callback_(audio_stream_);
  }

}

void Demuxer::NotifyCapacityAvailable() {
  DCHECK(task_runner_.BelongsToCurrentThread());
  task_runner_.PostTask(FROM_HERE, [this]() {
    DemuxTask();
  });
}

void Demuxer::DemuxTask() {
  DCHECK(task_runner_.BelongsToCurrentThread());
  DCHECK(audio_stream_);

  std::unique_ptr<AVPacket, AVPacketDeleter> packet(new AVPacket());
  if (ffmpeg::ReadFrameAndDiscardEmpty(format_context_, packet.get()) < 0) {
    audio_stream_->SetEndOfStream();
    return;
  }
  audio_stream_->EnqueuePacket(std::move(packet));

  if (audio_stream_->HasAvailableCapacity()) {
    host_->OnDemuxerHasEnoughData();
  }

  task_runner_.PostTask(FROM_HERE_WITH_EXPLICIT_FUNCTION("DemuxTaskLoop"), [this]() {
    DemuxTask();
  });
}

} // lychee