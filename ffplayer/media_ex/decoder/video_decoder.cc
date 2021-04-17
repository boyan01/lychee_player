//
// Created by yangbin on 2021/4/8.
//

#include "decoder/video_decoder.h"

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/bind_to_current_loop.h"

#include "ffmpeg/ffmpeg_utils.h"

namespace media {

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() = default;

std::string VideoDecoder::GetDisplayName() {
  return "VideoDecoder";
}

void VideoDecoder::Initialize(const VideoDecoderConfig &decoder_config,
                              bool low_delay,
                              const OutputCallback &output_callback,
                              DecodeInitCallback init_callback) {
  DCHECK(output_callback);
  DCHECK(decoder_config.IsValidConfig());

  auto init_callback_bound = BindToCurrentLoop(std::move(init_callback));

  if (!ConfigureDecoder(decoder_config, low_delay)) {
    std::move(init_callback_bound)(false);
    return;
  }

  output_cb_ = output_callback;
  config_ = decoder_config;
  state_ = kNormal;
  std::move(init_callback_bound)(true);
}

void VideoDecoder::Decode(
    const std::shared_ptr<DecoderBuffer> &buffer,
    const VideoDecoder::DecodeCallback &decode_callback) {
  DCHECK(buffer.get());
  DCHECK(decode_callback);
  CHECK_NE(state_, kUninitialized);

  auto decode_cb_bound = BindToCurrentLoop(decode_callback);

  if (state_ == kError) {
    std::move(decode_cb_bound)(DecodeStatus::DECODE_ERROR);
    return;
  }

  if (state_ == kDecodeFinished) {
    std::move(decode_cb_bound)(DecodeStatus::OK);
    return;
  }

  DCHECK_EQ(state_, kNormal);

  // During decode, because reads are issued asynchronously, it is possible to
  // receive multiple end of stream buffers since each decode is acked. There
  // are three states the decoder can be in:
  //
  //   kNormal: This is the starting state. Buffers are decoded. Decode errors
  //            are discarded.
  //   kDecodeFinished: All calls return empty frames.
  //   kError: Unexpected error happened.
  //
  // These are the possible state transitions.
  //
  // kNormal -> kDecodeFinished:
  //     When EOS buffer is received and the codec has been flushed.
  // kNormal -> kError:
  //     A decoding error occurs and decoding needs to stop.
  // (any state) -> kNormal:
  //     Any time Reset() is called.

  if (!FFmpegDecode(*buffer)) {
    state_ = kError;
    std::move(decode_cb_bound)(DecodeStatus::DECODE_ERROR);
    return;
  }

  if (buffer->isEndOfStream()) {
    state_ = kDecodeFinished;
  }

  // VideoDecoderShim expects that |decode_cb| is called only after
  // |output_cb_|.
  std::move(decode_cb_bound)(DecodeStatus::OK);

}

bool VideoDecoder::FFmpegDecode(const DecoderBuffer &buffer) {
  AVPacket packet;
  av_init_packet(&packet);

  if (buffer.isEndOfStream()) {
    packet.data = nullptr;
    packet.size = 0;
  } else {
    packet.data = const_cast<uint8_t *>(buffer.GetData());
    packet.size = buffer.GetDataSize();

    DCHECK(packet.data);
    DCHECK_GT(packet.size, 0);

    // Let FFmpeg handle presentation timestamp reordering.
    codec_context_->reordered_opaque = buffer.GetTimestamp().count();
  }

  switch (decoding_loop_->DecodePacket(&packet, [this](AVFrame *frame) { return OnNewFrame(frame); })) {
    case FFmpegDecodingLoop::DecodeStatus::kOkay:break;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      LOG(ERROR) << "Failed to send video packet for decoding: ";
      return false;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      LOG(ERROR) << GetDisplayName() << " failed to decode a video frame: "
                 << ffmpeg::AVErrorToString(decoding_loop_->last_av_error_code()) << ", at ";
      return false;
    }
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed: {
      return false;
    }
  }

  return true;
}

bool VideoDecoder::OnNewFrame(AVFrame *frame) {
  auto video_frame = VideoFrame::Create(frame, codec_context_->time_base);
  output_cb_(video_frame);
  return true;
}

void VideoDecoder::ReleaseFFmpegResources() {
  decoding_loop_.reset();
  codec_context_.reset();
}

bool VideoDecoder::ConfigureDecoder(const VideoDecoderConfig &config, bool low_delay) {
  DCHECK(config.IsValidConfig());

  // Release existing decoder resources if necessary.
  ReleaseFFmpegResources();

  codec_context_.reset(avcodec_alloc_context3(nullptr));
  ffmpeg::VideoDecoderConfigToAVCodecContext(config, codec_context_.get());

  codec_context_->thread_count = FF_THREAD_SLICE | (low_delay ? 0 : FF_THREAD_FRAME);

  auto *codec = avcodec_find_decoder(codec_context_->codec_id);
  if (!codec || avcodec_open2(codec_context_.get(), codec, nullptr) < 0) {
    ReleaseFFmpegResources();
    return false;
  }

  decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(codec_context_.get());
  return true;
}

}