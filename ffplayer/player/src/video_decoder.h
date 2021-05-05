//
// Created by yangbin on 2021/5/3.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_DECODER_H_
#define MEDIA_PLAYER_SRC_VIDEO_DECODER_H_

#include "memory"

#include "base/basictypes.h"

extern "C" {
#include "libavformat/avformat.h"
}

#include "ffmpeg_decoding_loop.h"
#include "video_frame.h"
#include "demuxer_stream.h"

namespace media {

class VideoDecoder2 {

 public:
  VideoDecoder2();

  virtual ~VideoDecoder2();

  using OutputCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

  int Initialize(VideoDecodeConfig video_decode_config, DemuxerStream *stream, OutputCallback output_callback);

  void Decode(const AVPacket *packet);

 private:

  std::unique_ptr<FFmpegDecodingLoop> ffmpeg_decoding_loop_;
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context_;
  DemuxerStream *stream_ = nullptr;
  OutputCallback output_callback_;

  VideoDecodeConfig video_decode_config_;

  bool OnFrameAvailable(AVFrame *frame);

  DISALLOW_COPY_AND_ASSIGN(VideoDecoder2);

};

}

#endif //MEDIA_PLAYER_SRC_VIDEO_DECODER_H_
