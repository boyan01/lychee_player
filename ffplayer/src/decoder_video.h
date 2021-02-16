//
// Created by boyan on 2021/2/14.
//

#ifndef FFP_DECODER_VIDEO_H
#define FFP_DECODER_VIDEO_H

#include "decoder_base.h"
#include "render_video_base.h"

class VideoDecoder : public Decoder<VideoRenderBase> {

 private:

  int GetVideoFrame(AVFrame *frame);

 protected:

  const char *debug_label() override;

  int DecodeThread() override;

 public:
  VideoDecoder(unique_ptr_d<AVCodecContext> codecContext,
               std::unique_ptr<DecodeParams> decodeParams,
               std::shared_ptr<VideoRenderBase> render);
};

#endif //FFP_DECODER_VIDEO_H
