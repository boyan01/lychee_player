#define LOG_TAG "ffmpeg_audio"

#include "backends/ffmpeg/ffmpeg_audio_decoder.h"

#include "base/log.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace lychee {

FFmpegAudioDecoder::FFmpegAudioDecoder()
    : frame_(av_frame_alloc()), av_packet_(av_packet_alloc()) {}

FFmpegAudioDecoder::~FFmpegAudioDecoder() {
  swr_free(&swr_context_);
  av_channel_layout_uninit(&swr_layout_);
}

bool FFmpegAudioDecoder::Initialize(const AudioConfig& config,
                                    const AudioOutputFormat& output_format,
                                    std::string* error) {
  output_format_ = output_format;
  AVCodecParameters* parameters = CreateCodecParameters(config);
  const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
  if (!codec) {
    avcodec_parameters_free(&parameters);
    *error = "no decoder for " + config.codec_name;
    return false;
  }
  context_.reset(avcodec_alloc_context3(codec));
  int ret = avcodec_parameters_to_context(context_.get(), parameters);
  avcodec_parameters_free(&parameters);
  if (ret < 0) {
    *error = "avcodec_parameters_to_context: " + AVErrorString(ret);
    return false;
  }
  context_->pkt_timebase = kMicrosTimeBase;
  ret = avcodec_open2(context_.get(), codec, nullptr);
  if (ret < 0) {
    *error = "avcodec_open2: " + AVErrorString(ret);
    return false;
  }
  LOGI("decoder opened for %s %d Hz %d ch -> %d Hz %d ch", codec->name,
       config.sample_rate, config.channels, output_format.sample_rate,
       output_format.channels);
  return true;
}

bool FFmpegAudioDecoder::Decode(const EncodedPacket& packet,
                                const OutputCallback& output) {
  int ret;
  if (packet.end_of_stream) {
    ret = avcodec_send_packet(context_.get(), nullptr);
  } else {
    ret = CopyToAVPacket(packet, av_packet_.get());
    if (ret >= 0) {
      ret = avcodec_send_packet(context_.get(), av_packet_.get());
      av_packet_unref(av_packet_.get());
    }
  }
  if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
    LOGW("avcodec_send_packet: %s", AVErrorString(ret).c_str());
  }
  return ReceiveFrames(output);
}

bool FFmpegAudioDecoder::ReceiveFrames(const OutputCallback& output) {
  for (;;) {
    int ret = avcodec_receive_frame(context_.get(), frame_.get());
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;
    }
    if (ret < 0) {
      LOGW("avcodec_receive_frame: %s", AVErrorString(ret).c_str());
      return true;
    }
    std::shared_ptr<AudioBuffer> buffer = Resample(frame_.get());
    av_frame_unref(frame_.get());
    if (buffer) {
      output(std::move(buffer));
    }
  }
}

bool FFmpegAudioDecoder::EnsureResampler(const AVFrame* frame) {
  if (swr_context_ && swr_format_ == frame->format &&
      swr_rate_ == frame->sample_rate &&
      av_channel_layout_compare(&swr_layout_, &frame->ch_layout) == 0) {
    return true;
  }
  swr_free(&swr_context_);
  AVChannelLayout output_layout;
  av_channel_layout_default(&output_layout, output_format_.channels);
  int ret = swr_alloc_set_opts2(
      &swr_context_, &output_layout, AV_SAMPLE_FMT_FLT,
      output_format_.sample_rate, &frame->ch_layout,
      static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
      nullptr);
  av_channel_layout_uninit(&output_layout);
  if (ret >= 0) {
    ret = swr_init(swr_context_);
  }
  if (ret < 0) {
    LOGE("resampler init failed: %s", AVErrorString(ret).c_str());
    swr_free(&swr_context_);
    return false;
  }
  av_channel_layout_uninit(&swr_layout_);
  av_channel_layout_copy(&swr_layout_, &frame->ch_layout);
  swr_format_ = frame->format;
  swr_rate_ = frame->sample_rate;
  return true;
}

std::shared_ptr<AudioBuffer> FFmpegAudioDecoder::Resample(AVFrame* frame) {
  if (!EnsureResampler(frame)) {
    return nullptr;
  }
  // Samples still buffered in the resampler come out first.
  int64_t delay = swr_get_delay(swr_context_, kMicrosPerSecond);
  int64_t pts = frame->best_effort_timestamp;
  if (pts == AV_NOPTS_VALUE) {
    pts = next_pts_;
  } else {
    pts -= delay;
  }

  int capacity = swr_get_out_samples(swr_context_, frame->nb_samples);
  auto buffer = std::make_shared<AudioBuffer>();
  buffer->sample_rate = output_format_.sample_rate;
  buffer->channels = output_format_.channels;
  buffer->samples.resize(static_cast<size_t>(capacity) *
                         output_format_.channels);
  uint8_t* out[] = {reinterpret_cast<uint8_t*>(buffer->samples.data())};
  int converted =
      swr_convert(swr_context_, out, capacity,
                  const_cast<const uint8_t**>(frame->extended_data),
                  frame->nb_samples);
  if (converted < 0) {
    LOGE("swr_convert: %s", AVErrorString(converted).c_str());
    return nullptr;
  }
  buffer->samples.resize(static_cast<size_t>(converted) *
                         output_format_.channels);
  buffer->pts = pts;
  if (pts != kNoTimestamp) {
    next_pts_ = pts + buffer->duration();
  }
  return converted > 0 ? buffer : nullptr;
}

void FFmpegAudioDecoder::Flush() {
  avcodec_flush_buffers(context_.get());
  // Drop samples buffered from before the flush.
  swr_free(&swr_context_);
  next_pts_ = kNoTimestamp;
}

}  // namespace lychee
