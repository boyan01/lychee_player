//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_DECODER_AUDIO_H
#define FFP_DECODER_AUDIO_H

#include "decoder_base.h"
#include "render_audio_base.h"
#include "ffp_define.h"
#include "audio_render_basic.h"

namespace media {

class AudioDecoder : public media::Decoder {

 private:
  std::shared_ptr<BasicAudioRender> audio_render_;

 protected:

  const char *debug_label() override;

  int DecodeThread() override;

 public:

  AudioDecoder(
      unique_ptr_d<AVCodecContext> codec_context_,
      std::unique_ptr<media::DecodeParams> decode_params_,
      std::shared_ptr<BasicAudioRender> audio_render,
      std::function<void()> on_decoder_blocking
  );

 protected:

  void AbortRender() override;

};

}
#endif //FFP_DECODER_AUDIO_H
