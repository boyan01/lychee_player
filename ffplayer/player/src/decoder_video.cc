//
// Created by boyan on 2021/2/14.
//

#include "decoder_video.h"

#include <utility>

#include "base/logging.h"
#include "base/lambda.h"

namespace media {

VideoDecoder::VideoDecoder(TaskRunner *task_runner) : decode_task_runner_(task_runner) {}

VideoDecoder::~VideoDecoder() = default;

int VideoDecoder::Initialize(VideoDecodeConfig config, DemuxerStream *stream) {
  DCHECK(!video_codec_context_);

  video_codec_context_ = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>(avcodec_alloc_context3(nullptr));

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
  DLOG(INFO) << "ReadFrame";
  DCHECK(!read_callback_);
  read_callback_ = std::move(read_callback);
  decode_task_runner_->PostTask(FROM_HERE, bind_weak(&VideoDecoder::VideoDecodeTask, shared_from_this()));
}

void VideoDecoder::VideoDecodeTask() {
  DLOG(INFO) << "VideoDecodeTask";

  DCHECK(decode_task_runner_->BelongsToCurrentThread());
  DCHECK(video_stream_);
  DCHECK(video_decoding_loop_);

  AVPacket packet;
  av_init_packet(&packet);
  if (!video_stream_->ReadPacket(&packet) || packet.data == PacketQueue::GetFlushPacket()->data) {
    std::move(read_callback_)(VideoFrame::Empty());
    return;
  }

  switch (video_decoding_loop_->DecodePacket(
      &packet, bind_weak(&VideoDecoder::OnNewFrameAvailable, shared_from_this(), false))) {
    case FFmpegDecodingLoop::DecodeStatus::kOkay:return;
    case FFmpegDecodingLoop::DecodeStatus::kFrameProcessingFailed :return;
    default: {
      break;
    }
  }
  std::move(read_callback_)(VideoFrame::Empty());
}

bool VideoDecoder::OnNewFrameAvailable(AVFrame *frame) {
  auto frame_rate = video_decode_config_.frame_rate();
  auto duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational{frame_rate.den, frame_rate.num}) : 0);
  auto pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(video_decode_config_.time_base());
  VideoFrame video_frame(frame, pts, duration, 0);
  std::move(read_callback_)(video_frame);
  return false;
}

}
