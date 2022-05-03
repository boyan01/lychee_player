//
// Created by boyan on 22-5-3.
//

#include "audio_decoder.h"
#include "ffmpeg_utils.h"

namespace lychee {

AudioDecoder::AudioDecoder(const media::TaskRunner &task_runner) : task_runner_(task_runner) {

}

void AudioDecoder::Initialize(
    DemuxerStream *demuxer_stream,
    AudioDecoder::OutputCallback output_callback,
    InitializedCallback initialized_callback
) {
  task_runner_.PostTask(FROM_HERE, [&]() {
    DCHECK(!codec_context_) << "codec context already initialized.";

    output_callback_ = std::move(output_callback);
    stream_ = demuxer_stream;

    codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(
        avcodec_alloc_context3(nullptr));

    auto config = demuxer_stream->audio_decode_config();

    auto ret = avcodec_parameters_to_context(codec_context_.get(), &config.codec_parameters());
    DCHECK_GE(ret, 0);
    codec_context_->codec_id = config.CodecId();
    codec_context_->pkt_timebase = config.time_base();

    auto *codec = avcodec_find_decoder(config.CodecId());
    DCHECK(codec) << "no decoder could be found for CodecId"
                  << avcodec_get_name(config.CodecId());

    if (codec == nullptr) {
      codec_context_.reset();
      initialized_callback(false);
      return;
    }

    codec_context_->codec_id = codec->id;

    // TODO low_res and fast flag.
    ret = avcodec_open2(codec_context_.get(), codec, nullptr);
    DCHECK_GE(ret, 0) << "can not open av codec, reason: "
                      << ffmpeg::AVErrorToString(ret);
    if (ret < 0) {
      codec_context_.reset();
      initialized_callback(false);
      return;
    }

    ffmpeg_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(codec_context_.get(), true);
    initialized_callback(true);
  });
}

// static
int64 AudioDecoder::GetChannelLayout(AVFrame *av_frame) {
  auto valid = av_frame->channel_layout
      && av_frame->channels == av_get_channel_layout_nb_channels(av_frame->channel_layout);
  return valid ? int64(av_frame->channel_layout) : av_get_default_channel_layout(av_frame->channels);
}

void AudioDecoder::Decode(const std::shared_ptr<DecoderBuffer> &decoder_buffer) {
  DCHECK(stream_);
  DCHECK(ffmpeg_decoding_loop_);
  switch (ffmpeg_decoding_loop_->DecodePacket(decoder_buffer->av_packet(), [this](AVFrame *frame) -> bool {
    return OnFrameAvailable(frame);
  })) {
    case FFmpegDecodingLoop::DecodeStatus::kOkay: {
      break;
    }
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      DLOG(ERROR) << "Failed to send video packet for decoding";
      break;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      DLOG(ERROR) << " failed to decode a video frame: "
                  << ffmpeg::AVErrorToString(
                      ffmpeg_decoding_loop_->last_av_error_code());
      break;
    }
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed: {
      return;
    }
  }
}

bool AudioDecoder::OnFrameAvailable(AVFrame *av_frame) {

  if (!swr_context_) {
    auto decode_channel_layout = GetChannelLayout(av_frame);
    swr_context_ = swr_alloc_set_opts(
        nullptr, audio_device_info_.channel_layout, audio_device_info_.fmt,
        audio_device_info_.freq, decode_channel_layout,
        AVSampleFormat(av_frame->format), av_frame->sample_rate, 0, nullptr);

    if (!swr_context_ || swr_init(swr_context_) < 0) {
      DLOG(ERROR) << "Cannot create sample rate converter for conversion of "
                  << av_frame->sample_rate << " Hz "
                  << av_get_sample_fmt_name(
                      static_cast<AVSampleFormat>(av_frame->format))
                  << av_frame->channels << " channels to "
                  << audio_device_info_.freq << " Hz "
                  << av_get_sample_fmt_name(audio_device_info_.fmt) << " "
                  << audio_device_info_.channels << " channels!";
      swr_free(&swr_context_);
      return false;
    }
  }

  uint8 *data;
  int data_size;

  const auto **in = (const uint8_t **) av_frame->extended_data;
  int out_count = av_frame->nb_samples *
      audio_decode_config_.samples_per_second() / av_frame->sample_rate + 256;
  int out_size = av_samples_get_buffer_size(nullptr, audio_decode_config_.channels(),
                                            out_count, audio_device_info_.fmt, 0);
  DCHECK_GE(out_size, 0) << "av_samples_get_buffer_size() failed";
  data = static_cast<uint8 *>(malloc(sizeof(uint8) * out_size));
  auto out_nb_samples = swr_convert(swr_context_, &data, out_size, in, av_frame->nb_samples);
  if (out_nb_samples < 0) {
    DLOG(ERROR) << "swr_convert failed";
    return false;
  }
  if (out_nb_samples == out_count) {
    DLOG(WARNING) << "audio buffer is probably too small";
    if (swr_init(swr_context_) < 0) {
      swr_free(&swr_context_);
    }
  }
  data_size = out_nb_samples * audio_device_info_.channels * av_get_bytes_per_sample(audio_device_info_.fmt);

  double pts = av_frame->pts == AV_NOPTS_VALUE ? NAN : av_q2d(audio_decode_config_.time_base()) * double(av_frame->pts);

  output_callback_(std::make_shared<AudioBuffer>(
      data, data_size, pts,
      audio_device_info_.bytes_per_sec));
  return true;
}

}
