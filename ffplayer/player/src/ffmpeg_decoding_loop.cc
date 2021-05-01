//
// Created by yangbin on 2021/4/8.
//

#include "ffmpeg_decoding_loop.h"

#include "base/logging.h"
#include "ffp_utils.h"

namespace media {

FFmpegDecodingLoop::FFmpegDecodingLoop(AVCodecContext *context, bool continue_on_decoding_errors)
    : context_(context),
      continue_on_decoding_errors_(continue_on_decoding_errors),
      frame_(av_frame_alloc()) {

}

FFmpegDecodingLoop::~FFmpegDecodingLoop() = default;

FFmpegDecodingLoop::DecodeStatus FFmpegDecodingLoop::DecodePacket(
    const AVPacket *packet,
    const FrameReadyCB &frame_ready_cb) {
  bool sent_packet = false, frames_remaining = true, decoder_error = false;
  while (!sent_packet || frames_remaining) {
    if (!sent_packet) {
      const int result = avcodec_send_packet(context_, packet);
      if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        DLOG(ERROR) << "Failed to send packet for decoding: " << result;
        return DecodeStatus::kSendPacketFailed;
      }

      sent_packet = result != AVERROR(EAGAIN);
    }

    // See if any frames are available. If we receive an EOF or EAGAIN, there
    // should be nothing left to do this pass since we've already provided the
    // only input packet that we have.
    const int result = avcodec_receive_frame(context_, frame_.get());
    if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
      frames_remaining = false;

      // TODO(dalecurtis): This should be a DCHECK() or MEDIA_LOG, but since
      // this API is new, lets make it a CHECK first and monitor reports.
      if (result == AVERROR(EAGAIN)) {
        CHECK(sent_packet) << "avcodec_receive_frame() and "
                              "avcodec_send_packet() both returned EAGAIN, "
                              "which is an API violation.";
      }

      continue;
    } else if (result < 0) {
      DLOG(ERROR) << "Failed to decode frame: " << result;
      last_av_error_code_ = result;
      if (!continue_on_decoding_errors_)
        return DecodeStatus::kDecodeFrameFailed;
      decoder_error = true;
      continue;
    }

    const bool frame_processing_success = frame_ready_cb(frame_.get());
    av_frame_unref(frame_.get());
    if (!frame_processing_success)
      return DecodeStatus::kFrameProcessingFailed;
  }

  return decoder_error ? DecodeStatus::kDecodeFrameFailed : DecodeStatus::kOkay;
}

} // namespace media