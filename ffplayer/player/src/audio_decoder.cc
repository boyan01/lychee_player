//
// Created by yangbin on 2021/5/2.
//

#include "audio_decoder.h"

#include "base/logging.h"
#include "base/lambda.h"

#include "ffp_utils.h"

namespace media {

AudioDecoder::AudioDecoder() : audio_decode_config_(), audio_device_info_() {

}

AudioDecoder::~AudioDecoder() = default;

int AudioDecoder::Initialize(const AudioDecodeConfig &config, DemuxerStream *stream, OutputCallback output_callback) {
  DCHECK(!codec_context_);

  audio_decode_config_ = config;
  codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));
  output_callback_ = std::move(output_callback);

  audio_device_info_.fmt = AV_SAMPLE_FMT_S16;
  audio_device_info_.channels = config.channels();
  audio_device_info_.freq = config.samples_per_second();
  audio_device_info_.channel_layout = int64(config.channel_layout());
  audio_device_info_.frame_size = av_samples_get_buffer_size(
      nullptr, audio_device_info_.channels, 1, audio_device_info_.fmt, 1);
  audio_device_info_.bytes_per_sec = av_samples_get_buffer_size(
      nullptr, audio_device_info_.channels, audio_device_info_.freq, audio_device_info_.fmt, 1);

  auto ret = avcodec_parameters_to_context(codec_context_.get(), &config.codec_parameters());
  DCHECK_GE(ret, 0);

  codec_context_->codec_id = config.CodecId();
  codec_context_->pkt_timebase = config.time_base();

  auto *codec = avcodec_find_decoder(config.CodecId());
  DCHECK(codec) << "No decoder could be found for CodecId: " << avcodec_get_name(config.CodecId());

  if (codec == nullptr) {
    codec_context_.reset();
    return -1;
  }

  codec_context_->codec_id = codec->id;
  // TODO low_res and fast flag.

  ret = avcodec_open2(codec_context_.get(), codec, nullptr);
  DCHECK_GE(ret, 0) << "can not open avcodec, reason: " << av_err_to_str(ret);
  if (ret < 0) {
    codec_context_.reset();
    return ret;
  }

  ffmpeg_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(codec_context_.get(), true);

  stream_ = stream;

  return 0;
}

void AudioDecoder::Decode(const AVPacket *packet) {
  DCHECK(stream_);
  DCHECK(ffmpeg_decoding_loop_);

  switch (ffmpeg_decoding_loop_->DecodePacket(
      packet, std::bind(&AudioDecoder::OnFrameAvailable, this, std::placeholders::_1))) {
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed :return;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      DLOG(ERROR) << "Failed to send video packet for decoding";
      return;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      DLOG(ERROR) << " failed to decode a video frame: "
                  << av_err_to_str(ffmpeg_decoding_loop_->last_av_error_code());
      return;
    }
    case FFmpegDecodingLoop::DecodeStatus::kOkay:break;
  }
}

bool AudioDecoder::OnFrameAvailable(AVFrame *frame) {

  if (!swr_ctx_) {

    auto decode_channel_layout = GetChannelLayout(frame);

    swr_ctx_ = swr_alloc_set_opts(
        nullptr,
        audio_device_info_.channel_layout, audio_device_info_.fmt, audio_device_info_.freq,
        decode_channel_layout, AVSampleFormat(frame->format), frame->sample_rate,
        0, nullptr
    );

    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
      DLOG(ERROR) << "Cannot create sample rate converter for conversion of "
                  << frame->sample_rate << " Hz "
                  << av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format))
                  << frame->channels << " channels to "
                  << audio_device_info_.freq << " Hz "
                  << av_get_sample_fmt_name(audio_device_info_.fmt) << "%s "
                  << audio_device_info_.channels << " channels!";
      swr_free(&swr_ctx_);
      return false;
    }
  }

  uint8 *data;
  int data_size;

  if (swr_ctx_) {
    const auto **in = (const uint8_t **) frame->extended_data;
    int out_count = frame->nb_samples * audio_decode_config_.samples_per_second() / frame->sample_rate + 256;
    int out_size = av_samples_get_buffer_size(
        nullptr, audio_decode_config_.channels(), out_count, AV_SAMPLE_FMT_S16, 0);
    DCHECK_GT(out_size, 0) << "av_samples_get_buffer_size() failed";
    data = static_cast<uint8 *>(malloc(sizeof(uint8) * out_size));
    auto out_nb_samples = swr_convert(swr_ctx_, &data, out_size, in, frame->nb_samples);
    if (out_nb_samples < 0) {
      DLOG(ERROR) << "swr_convert Failed";
      return false;
    }
    if (out_nb_samples == out_count) {
      DLOG(WARNING) << "audio buffer is probably too small";
      if (swr_init(swr_ctx_) < 0) {
        swr_free(&swr_ctx_);
      }
    }
    data_size = out_nb_samples * audio_device_info_.channels * av_get_bytes_per_sample(audio_device_info_.fmt);
  } else {
    data_size = av_samples_get_buffer_size(nullptr, frame->channels, frame->nb_samples,
                                           AVSampleFormat(frame->format), 1);
    data = static_cast<uint8 *>(malloc(sizeof(uint8) * data_size));
    memcpy(data, frame->data[0], data_size);
  }

  output_callback_(std::make_shared<AudioBuffer>(
      data, data_size, av_q2d(audio_decode_config_.time_base()) * double(frame->pts),
      audio_device_info_.bytes_per_sec));
  return true;
}

// static
int64 AudioDecoder::GetChannelLayout(AVFrame *frame) {
  bool valid = frame->channel_layout && frame->channels == av_get_channel_layout_nb_channels(frame->channel_layout);
  return valid ? int64(frame->channel_layout) : av_get_default_channel_layout(frame->channels);
}

} // namespace media