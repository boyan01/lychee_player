//
// Created by yangbin on 2021/5/2.
//

#include "audio_decoder.h"

#include "base/logging.h"
#include "base/bind_to_current_loop.h"
#include "base/lambda.h"

#include "ffp_utils.h"

namespace media {

AudioDecoder2::AudioDecoder2() : audio_decode_config_() {

}

AudioDecoder2::~AudioDecoder2() {

}

int AudioDecoder2::Initialize(const AudioDecodeConfig &config, DemuxerStream *stream, OutputCallback output_callback) {
  DCHECK(!codec_context_);

  audio_decode_config_ = config;
  codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));
  output_callback_ = std::move(output_callback);

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
  stream_->stream()->discard = AVDISCARD_DEFAULT;
  stream->packet_queue()->Start();

  return 0;
}

void AudioDecoder2::Decode(const AVPacket *packet) {
  DCHECK(stream_);
  DCHECK(ffmpeg_decoding_loop_);

  switch (ffmpeg_decoding_loop_->DecodePacket(
      packet, std::bind(&AudioDecoder2::OnFrameAvailable, this, std::placeholders::_1))) {
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

bool AudioDecoder2::OnFrameAvailable(AVFrame *frame) {
  auto bytes_per_sec = av_samples_get_buffer_size(nullptr, frame->channels, frame->sample_rate,
                                                  AV_SAMPLE_FMT_S16, 1);

//  if (!swr_ctx || swr_init(swr_ctx) < 0) {
//    DLOG(ERROR) << "Cannot create sample rate converter for conversion of "
//                << frame->sample_rate << " Hz "
//                << av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format))
//                << frame->channels << " channels to "
//                << audio_decode_config_.samples_per_second() << " Hz "
//                << av_get_sample_fmt_name(AV_SAMPLE_FMT_S16) << "%s "
//                << audio_decode_config_.codec_parameters().channels << " channels!";
//    swr_free(&swr_ctx);
//    return false;
//  }
//
//  if (swr_ctx) {
//    const auto **in = (const uint8_t **) frame->extended_data;
//    int64_t out_count = frame->nb_samples * audio_decode_config_.samples_per_second() / frame->sample_rate + 256;
//    int out_size = av_samples_get_buffer_size(nullptr,
//                                              audio_decode_config_.channels(), out_count, AV_SAMPLE_FMT_S16, 0);
//    DCHECK_GT(out_size, 0) << "av_samples_get_buffer_size() failed";
//
//  }

  auto size = frame->linesize[0];

  uint8 *data = static_cast<uint8 *>(malloc(sizeof(uint8) * size));
  memcpy(data, frame->data[0], size);
  output_callback_(std::make_shared<AudioBuffer>(
      data, size, frame->pts, bytes_per_sec
  ));

  return true;
}

} // namespace media