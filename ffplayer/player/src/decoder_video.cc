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
    : task_runner_(task_runner), frame_queue_(kVideoPictureQueueSize) {
}

VideoDecoder::~VideoDecoder() = default;

int VideoDecoder::Initialize(VideoDecodeConfig config, DemuxerStream *stream, OnFlushCallback on_flush_callback) {
  DCHECK(!codec_context_);
  DCHECK(frame_queue_.IsEmpty());

  codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));
  on_flush_callback_ = std::move(on_flush_callback);

  auto ret = avcodec_parameters_to_context(codec_context_.get(), &config.codec_parameters());
  DCHECK_GE(ret, 0);

  codec_context_->codec_id = config.codec_id();
  codec_context_->pkt_timebase = config.time_base();

  auto *codec = avcodec_find_decoder(config.codec_id());
  DCHECK(codec) << "No decoder could be found for CodecId:" << avcodec_get_name(config.codec_id());

  if (codec == nullptr) {
    codec_context_.reset();
    return -1;
  }

  codec_context_->codec_id = codec->id;
  int stream_lower = config.low_res();
  DCHECK_LE(stream_lower, codec->max_lowres)
      << "The maximum value for lowres supported by the decoder is " << codec->max_lowres
      << ", but is " << stream_lower;
  if (stream_lower > codec->max_lowres) {
    stream_lower = codec->max_lowres;
  }
  codec_context_->lowres = stream_lower;
  if (config.fast()) {
    codec_context_->flags2 |= AV_CODEC_FLAG2_FAST;
  }

  ret = avcodec_open2(codec_context_.get(), codec, nullptr);
  DCHECK_GE(ret, 0) << "can not open avcodec, reason: " << av_err_to_str(ret);
  if (ret < 0) {
    codec_context_.reset();
    return ret;
  }

  ffmpeg_decoding_loop_ = std::make_unique<FFmpegDecodingLoop>(codec_context_.get(), true);
//  video_render->SetMaxFrameDuration(video_decode_config_.max_frame_duration());
  video_decode_config_ = config;

  stream_ = stream;

  stream_->stream()->discard = AVDISCARD_DEFAULT;
  stream_->packet_queue()->Start();

  return 0;
}

void VideoDecoder::ReadFrame(VideoDecoder::ReadCallback read_callback) {
  auto read_callback_bound = BindToCurrentLoop(read_callback);

  if (frame_queue_.IsEmpty()) {
    read_callback_ = std::move(read_callback);
  } else {
    auto frame = frame_queue_.GetFront();
    frame_queue_.DeleteFront();
    read_callback_bound(std::move(frame));
  }

  task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
}

void VideoDecoder::VideoDecodeTask() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  DCHECK(stream_);
  DCHECK(ffmpeg_decoding_loop_);
  if (!NeedDecodeMore()) {
    return;
  }

  if (FFmpegDecode()) {
    if (read_callback_ && frame_queue_.GetFront()) {
      std::move(read_callback_)(std::move(frame_queue_.GetFront()));
      read_callback_ = nullptr;
      frame_queue_.DeleteFront();
      task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
    } else {
      if (NeedDecodeMore()) {
        task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
      }
    }
  } else {
    task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
  }
}

bool VideoDecoder::FFmpegDecode() {
  AVPacket packet;
  av_init_packet(&packet);

  if (!stream_->ReadPacket(&packet) || packet.data == PacketQueue::GetFlushPacket()->data) {
    frame_queue_.Clear();
    if (on_flush_callback_) {
      on_flush_callback_();
    }
    return false;
  }
  switch (ffmpeg_decoding_loop_->DecodePacket(
      &packet, std::bind(&VideoDecoder::OnNewFrameAvailable, this, std::placeholders::_1))) {
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed :return false;
    case FFmpegDecodingLoop::DecodeStatus::kSendPacketFailed: {
      DLOG(ERROR) << "Failed to send video packet for decoding";
      return false;
    }
    case FFmpegDecodingLoop::DecodeStatus::kDecodeFrameFailed: {
      DLOG(ERROR) << " failed to decode a video frame: "
                  << av_err_to_str(ffmpeg_decoding_loop_->last_av_error_code());
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
  frame_queue_.InsertLast(std::move(video_frame));
  return true;
}

bool VideoDecoder::NeedDecodeMore() {
  return !frame_queue_.IsFull();
}

}
