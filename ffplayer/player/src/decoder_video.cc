//
// Created by boyan on 2021/2/14.
//

#include "decoder_video.h"

#include <utility>

#include "base/logging.h"
#include "base/lambda.h"
#include "base/bind_to_current_loop.h"

namespace media {

const int kVideoPictureQueueSize = 3;

VideoDecoder::VideoDecoder(TaskRunner *task_runner)
    : decode_task_runner_(task_runner), picture_queue_(kVideoPictureQueueSize) {
}

VideoDecoder::~VideoDecoder() = default;

int VideoDecoder::Initialize(VideoDecodeConfig config, DemuxerStream *stream, OnFlushCallback on_flush_callback) {
  DCHECK(!video_codec_context_);
  DCHECK(picture_queue_.IsEmpty());

  video_codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));
  on_flush_callback_ = std::move(on_flush_callback);

  auto ret = avcodec_parameters_to_context(video_codec_context_.get(), &config.codec_parameters());
  DCHECK_GE(ret, 0);

  video_codec_context_->codec_id = config.codec_id();
  video_codec_context_->pkt_timebase = config.time_base();

  auto *codec = avcodec_find_decoder(config.codec_id());
  DCHECK(codec) << "No decoder could be found for CodecId:" << avcodec_get_name(config.codec_id());

  if (codec == nullptr) {
    video_codec_context_.reset();
    return -1;
  }

  video_codec_context_->codec_id = codec->id;
  int stream_lower = config.low_res();
  DCHECK_LE(stream_lower, codec->max_lowres)
      << "The maximum value for lowres supported by the decoder is " << codec->max_lowres
      << ", but is " << stream_lower;
  if (stream_lower > codec->max_lowres) {
    stream_lower = codec->max_lowres;
  }
  video_codec_context_->lowres = stream_lower;
  if (config.fast()) {
    video_codec_context_->flags2 |= AV_CODEC_FLAG2_FAST;
  }

  ret = avcodec_open2(video_codec_context_.get(), codec, nullptr);
  DCHECK_GE(ret, 0) << "can not open avcodec, reason: " << av_err_to_str(ret);
  if (ret < 0) {
    video_codec_context_.reset();
    return ret;
  }

  video_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(video_codec_context_.get(), true);
//  video_render->SetMaxFrameDuration(video_decode_config_.max_frame_duration());
  video_decode_config_ = config;

  video_stream_ = stream;

  video_stream_->stream()->discard = AVDISCARD_DEFAULT;
  video_stream_->packet_queue()->Start();

  return 0;
}

void VideoDecoder::ReadFrame(VideoDecoder::ReadCallback read_callback) {
  auto read_callback_bound = BindToCurrentLoop(read_callback);

  if (picture_queue_.IsEmpty()) {
    read_callback_ = std::move(read_callback);
  } else {
    auto frame = picture_queue_.GetFront();
    picture_queue_.DeleteFront();
    read_callback_bound(std::move(frame));
  }

  decode_task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
}

void VideoDecoder::VideoDecodeTask() {
  DCHECK(decode_task_runner_->BelongsToCurrentThread());
  DCHECK(video_stream_);
  DCHECK(video_decoding_loop_);
  if (!NeedDecodeMore()) {
    return;
  }

  if (FFmpegDecode()) {
    if (read_callback_ && picture_queue_.GetFront()) {
      std::move(read_callback_)(std::move(picture_queue_.GetFront()));
      read_callback_ = nullptr;
      picture_queue_.DeleteFront();
      decode_task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
    } else {
      if (NeedDecodeMore()) {
        decode_task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
      }
    }
  } else {
    decode_task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
  }
}

bool VideoDecoder::FFmpegDecode() {
  AVPacket packet;
  av_init_packet(&packet);

  if (!video_stream_->ReadPacket(&packet) || packet.data == PacketQueue::GetFlushPacket()->data) {
    picture_queue_.Clear();
    if (on_flush_callback_) {
      on_flush_callback_();
    }
    return false;
  }
  switch (video_decoding_loop_->DecodePacket(
      &packet, std::bind(&VideoDecoder::OnNewFrameAvailable, this, std::placeholders::_1))) {
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed :return false;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      DLOG(ERROR) << "Failed to send video packet for decoding";
      return false;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      DLOG(ERROR) << " failed to decode a video frame: "
                  << av_err_to_str(video_decoding_loop_->last_av_error_code());
      return false;
    }
    case FFmpegDecodingLoop::DecodeStatus::kOkay:break;
  }

  return true;
}

bool VideoDecoder::OnNewFrameAvailable(AVFrame *frame) {
  auto frame_rate = video_decode_config_.frame_rate();
  auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
  auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : double(frame->pts) * av_q2d(video_decode_config_.time_base());

  std::shared_ptr<VideoFrame> video_frame = std::make_shared<VideoFrame>(frame, pts, duration, 0);
  picture_queue_.InsertLast(std::move(video_frame));
  return true;
}

bool VideoDecoder::NeedDecodeMore() {
  return !picture_queue_.IsFull();
}

}
