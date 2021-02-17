//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_DECODER_AUDIO_H
#define FFP_DECODER_AUDIO_H

#include "decoder_base.h"
#include "render_audio_base.h"
#include "ffp_define.h"

class AudioDecoder : public Decoder<AudioRenderBase> {

 protected:

  const char *debug_label() override;

  int DecodeThread() override;

 public:
  AudioDecoder(unique_ptr_d<AVCodecContext> codec_context_,
               std::unique_ptr<DecodeParams> decode_params_,
               std::shared_ptr<AudioRenderBase> render_);
};

#endif //FFP_DECODER_AUDIO_H
