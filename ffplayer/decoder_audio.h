//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_DECODER_AUDIO_H
#define FFP_DECODER_AUDIO_H

#include "audio_render_basic.h"
#include "decoder_base.h"
#include "ffp_define.h"
#include "render_audio_base.h"

class AudioDecoder : public Decoder {
 private:
  std::shared_ptr<BasicAudioRender> audio_render_;

 protected:
  const char* debug_label() override;

  int DecodeThread() override;

 public:
  AudioDecoder(unique_ptr_d<AVCodecContext> codec_context_,
               std::unique_ptr<DecodeParams> decode_params_,
               std::shared_ptr<BasicAudioRender> audio_render,
               std::function<void()> on_decoder_blocking);

 protected:
  void AbortRender() override;
};

#endif  // FFP_DECODER_AUDIO_H
