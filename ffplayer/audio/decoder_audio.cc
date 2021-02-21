//
// Created by boyan on 2021/2/14.
//

#include "decoder_audio.h"

#include <utility>

const char *AudioDecoder::debug_label() {
  return "audio_decoder";
}

int AudioDecoder::DecodeThread() {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    return AVERROR(ENOMEM);
  }
  render->audio_queue_serial = &queue()->serial;
  do {
    auto got_frame = DecodeFrame(frame, nullptr);
    if (got_frame < 0) {
      break;
    }
    if (got_frame) {
      if (render->PushFrame(frame, pkt_serial) < 0) {
        break;
      }
    }
  } while (true);
  av_frame_free(&frame);
  return 0;
}

AudioDecoder::AudioDecoder(unique_ptr_d<AVCodecContext> codec_context_, std::unique_ptr<DecodeParams> decode_params_,
                           std::shared_ptr<AudioRenderBase> render_)
    : Decoder(std::move(codec_context_),
              std::move(decode_params_),
              std::move(render_)) {
  if (decode_params->audio_follow_stream_start_pts) {
    start_pts = decode_params->stream()->start_time;
    start_pts_tb = decode_params->stream()->time_base;
  }
  Start();
}
