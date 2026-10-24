#define LOG_TAG "ffmpeg_demuxer"

#include "backends/ffmpeg/ffmpeg_demuxer.h"

#include <cmath>

#include "backends/ffmpeg/ffmpeg_common.h"
#include "base/log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

namespace lychee {

namespace {

int64_t ToMicros(int64_t value, AVRational time_base) {
  if (value == AV_NOPTS_VALUE) {
    return kNoTimestamp;
  }
  return av_rescale_q(value, time_base, kMicrosTimeBase);
}

int RotationFromDisplayMatrix(const AVCodecParameters* parameters) {
  const AVPacketSideData* side_data = av_packet_side_data_get(
      parameters->coded_side_data, parameters->nb_coded_side_data,
      AV_PKT_DATA_DISPLAYMATRIX);
  if (!side_data || side_data->size < 9 * sizeof(int32_t)) {
    return 0;
  }
  // av_display_rotation_get() is counter-clockwise.
  double angle = -av_display_rotation_get(
      reinterpret_cast<const int32_t*>(side_data->data));
  if (std::isnan(angle)) {
    return 0;
  }
  int degrees = static_cast<int>(std::lround(angle / 90.0)) * 90;
  return ((degrees % 360) + 360) % 360;
}

}  // namespace

FFmpegDemuxer::FFmpegDemuxer() : av_packet_(av_packet_alloc()) {}

FFmpegDemuxer::~FFmpegDemuxer() {
  avformat_close_input(&format_context_);
  av_packet_free(&av_packet_);
}

int FFmpegDemuxer::InterruptCallback(void* opaque) {
  return static_cast<FFmpegDemuxer*>(opaque)->aborted_.load() ? 1 : 0;
}

bool FFmpegDemuxer::Open(const std::string& url, std::string* error) {
  format_context_ = avformat_alloc_context();
  format_context_->interrupt_callback.callback = &InterruptCallback;
  format_context_->interrupt_callback.opaque = this;

  int ret = avformat_open_input(&format_context_, url.c_str(), nullptr,
                                nullptr);
  if (ret < 0) {
    *error = "avformat_open_input: " + AVErrorString(ret);
    return false;
  }
  ret = avformat_find_stream_info(format_context_, nullptr);
  if (ret < 0) {
    *error = "avformat_find_stream_info: " + AVErrorString(ret);
    return false;
  }
  BuildMediaInfo();
  enabled_streams_.assign(format_context_->nb_streams, true);
  return true;
}

void FFmpegDemuxer::BuildMediaInfo() {
  AVFormatContext* ic = format_context_;
  media_info_.format_name = ic->iformat->name;
  if (ic->duration != AV_NOPTS_VALUE && ic->duration > 0) {
    media_info_.duration = ic->duration;  // AV_TIME_BASE is microseconds.
  }
  media_info_.start_time =
      ic->start_time == AV_NOPTS_VALUE ? 0 : ic->start_time;
  media_info_.seekable =
      ic->pb ? (ic->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0
             : media_info_.duration != kNoTimestamp;

  for (unsigned i = 0; i < ic->nb_streams; ++i) {
    AVStream* stream = ic->streams[i];
    AVCodecParameters* par = stream->codecpar;
    StreamInfo info;
    info.index = static_cast<int>(i);
    info.duration = ToMicros(stream->duration, stream->time_base);
    if (const AVDictionaryEntry* language =
            av_dict_get(stream->metadata, "language", nullptr, 0)) {
      info.language = language->value;
    }
    auto codec_private = std::make_shared<FFmpegCodecParams>(par);
    std::vector<uint8_t> extradata(par->extradata,
                                   par->extradata + par->extradata_size);

    if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
        !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
      info.type = MediaType::kVideo;
      VideoConfig& config = info.video;
      config.codec = VideoCodecFromAVCodecID(par->codec_id);
      config.codec_name = avcodec_get_name(par->codec_id);
      config.width = par->width;
      config.height = par->height;
      AVRational sar = av_guess_sample_aspect_ratio(ic, stream, nullptr);
      if (sar.num > 0 && sar.den > 0) {
        config.sar_num = sar.num;
        config.sar_den = sar.den;
      }
      config.rotation = RotationFromDisplayMatrix(par);
      config.color.primaries = static_cast<uint8_t>(par->color_primaries);
      config.color.transfer = static_cast<uint8_t>(par->color_trc);
      config.color.matrix = static_cast<uint8_t>(par->color_space);
      config.color.full_range = par->color_range == AVCOL_RANGE_JPEG;
      config.extradata = std::move(extradata);
      config.codec_private = codec_private;
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
      info.type = MediaType::kAudio;
      AudioConfig& config = info.audio;
      config.codec = AudioCodecFromAVCodecID(par->codec_id);
      config.codec_name = avcodec_get_name(par->codec_id);
      config.sample_rate = par->sample_rate;
      config.channels = par->ch_layout.nb_channels;
      config.extradata = std::move(extradata);
      config.codec_private = codec_private;
    } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      info.type = MediaType::kSubtitle;
    }
    media_info_.streams.push_back(std::move(info));
  }

  int video = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video >= 0 && media_info_.streams[video].type == MediaType::kVideo) {
    media_info_.default_video_stream = video;
  }
  int audio = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, video, nullptr,
                                  0);
  if (audio >= 0) {
    media_info_.default_audio_stream = audio;
  }
}

void FFmpegDemuxer::SetEnabledStreams(const std::vector<int>& stream_indices) {
  enabled_streams_.assign(format_context_->nb_streams, false);
  for (int index : stream_indices) {
    if (index >= 0 && index < static_cast<int>(enabled_streams_.size())) {
      enabled_streams_[index] = true;
    }
  }
  for (unsigned i = 0; i < format_context_->nb_streams; ++i) {
    format_context_->streams[i]->discard =
        enabled_streams_[i] ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
  }
}

DemuxerReadResult FFmpegDemuxer::Read(EncodedPacket* packet) {
  for (;;) {
    if (aborted_) {
      return DemuxerReadResult::kAborted;
    }
    int ret = av_read_frame(format_context_, av_packet_);
    if (ret == AVERROR(EAGAIN)) {
      continue;
    }
    if (ret == AVERROR_EOF) {
      return DemuxerReadResult::kEndOfStream;
    }
    if (ret < 0) {
      if (aborted_) {
        return DemuxerReadResult::kAborted;
      }
      LOGE("av_read_frame: %s", AVErrorString(ret).c_str());
      return DemuxerReadResult::kError;
    }
    int index = av_packet_->stream_index;
    if (index < 0 || index >= static_cast<int>(enabled_streams_.size()) ||
        !enabled_streams_[index]) {
      av_packet_unref(av_packet_);
      continue;
    }
    AVRational time_base = format_context_->streams[index]->time_base;
    packet->stream_index = index;
    packet->pts = ToMicros(av_packet_->pts, time_base);
    packet->dts = ToMicros(av_packet_->dts, time_base);
    packet->duration = av_packet_->duration > 0
                           ? ToMicros(av_packet_->duration, time_base)
                           : 0;
    packet->keyframe = (av_packet_->flags & AV_PKT_FLAG_KEY) != 0;
    packet->end_of_stream = false;
    packet->data.assign(av_packet_->data, av_packet_->data + av_packet_->size);
    av_packet_unref(av_packet_);
    return DemuxerReadResult::kOk;
  }
}

bool FFmpegDemuxer::Seek(int64_t time) {
  // Land on the keyframe at or before |time|; decoders skip ahead to |time|.
  int ret = avformat_seek_file(format_context_, -1, INT64_MIN, time, time, 0);
  if (ret < 0) {
    LOGW("seek to %lld backward failed: %s, retrying any direction",
         static_cast<long long>(time), AVErrorString(ret).c_str());
    ret = avformat_seek_file(format_context_, -1, INT64_MIN, time, INT64_MAX,
                             0);
  }
  if (ret < 0) {
    LOGE("seek to %lld failed: %s", static_cast<long long>(time),
         AVErrorString(ret).c_str());
    return false;
  }
  return true;
}

void FFmpegDemuxer::Abort() {
  aborted_ = true;
}

}  // namespace lychee
