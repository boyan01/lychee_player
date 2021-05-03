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
  output_callback_(std::make_shared<AudioBuffer>());

  return true;
}

} // namespace media