//
// Created by boyan on 2021/2/14.
//

#ifndef MEDIA_VIDEO_DECODER_H_
#define MEDIA_VIDEO_DECODER_H_

#include "decoder_base.h"
#include "render_video_base.h"

extern "C" {
#include "libavformat/avformat.h"
};

namespace media {

class VideoDecoder : public media::Decoder {

 private:

  std::shared_ptr<VideoRenderBase> video_render_;

  int GetVideoFrame(AVFrame *frame);

 protected:

  const char *debug_label() override;

  int DecodeThread() override;

 public:
  VideoDecoder(
      unique_ptr_d<AVCodecContext> codecContext,
      std::unique_ptr<media::DecodeParams> decodeParams,
      std::shared_ptr<VideoRenderBase> render,
      std::function<void()> on_decoder_blocking
  );

 protected:
  void AbortRender() override;
};

}

#endif //MEDIA_VIDEO_DECODER_H_
