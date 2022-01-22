//
// Created by yangbin on 2021/5/3.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_DECODER_H_
#define MEDIA_PLAYER_SRC_VIDEO_DECODER_H_

#include "base/basictypes.h"
#include "memory"

extern "C" {
#include "libavformat/avformat.h"
}

#include "demuxer_stream.h"
#include "ffmpeg_decoding_loop.h"
#include "video_frame.h"

namespace media {

class VideoDecoder {
 public:
  VideoDecoder();

  virtual ~VideoDecoder();

  using OutputCallback = std::function<void(std::shared_ptr<VideoFrame>)>;

  int Initialize(VideoDecodeConfig video_decode_config, DemuxerStream *stream,
                 OutputCallback output_callback);

  void Decode(std::shared_ptr<DecoderBuffer> decoder_buffer);

  void Flush();

 private:
  std::unique_ptr<FFmpegDecodingLoop> ffmpeg_decoding_loop_;
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context_;
  DemuxerStream *stream_ = nullptr;
  OutputCallback output_callback_;

  AVBufferRef *hw_device_context_;

  VideoDecodeConfig video_decode_config_;

  bool OnFrameAvailable(AVFrame *frame);

  DELETE_COPY_AND_ASSIGN(VideoDecoder);
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_VIDEO_DECODER_H_
