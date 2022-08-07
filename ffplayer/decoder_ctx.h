//
// Created by boyan on 2021/2/9.
//

#ifndef FFPLAYER_FFP_DECODER_H
#define FFPLAYER_FFP_DECODER_H

#include <condition_variable>
#include <functional>

#include "decoder_audio.h"
#include "decoder_video.h"
#include "ffp_define.h"
#include "ffp_frame_queue.h"
#include "ffp_packet_queue.h"
#include "render_base.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
};

class DecoderContext {
 public:
  /**
   * low resolution decoding, 1-> 1/2 size, 2->1/4 size
   */
  int low_res = 0;
  bool fast = false;

 private:
  AudioDecoder* audio_decoder = nullptr;
  VideoDecoder* video_decoder = nullptr;

  std::shared_ptr<BasicAudioRender> audio_render;
  std::shared_ptr<VideoRenderBase> video_render;

  std::shared_ptr<MediaClock> clock_ctx;

 private:
  int StartAudioDecoder(unique_ptr_d<AVCodecContext> codec_ctx,
                        std::unique_ptr<DecodeParams> decode_params);

 public:
  DecoderContext(std::shared_ptr<BasicAudioRender> audio_render_,
                 std::shared_ptr<VideoRenderBase> video_render_,
                 std::shared_ptr<MediaClock> clock_ctx_);

  ~DecoderContext();

  int StartDecoder(std::unique_ptr<DecodeParams> decode_params);

  bool AudioDecoderFinished() const {
    if (!audio_decoder) {
      return true;
    }
    return audio_decoder->IsFinished();
  }

  bool VideoDecoderFinished() const {
    if (!video_decoder) {
      return true;
    }
    return video_decoder->IsFinished();
  }
};

#endif  // FFPLAYER_FFP_DECODER_H
