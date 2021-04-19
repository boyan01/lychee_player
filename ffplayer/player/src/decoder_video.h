//
// Created by boyan on 2021/2/14.
//

#ifndef MEDIA_VIDEO_DECODER_H_
#define MEDIA_VIDEO_DECODER_H_

#include "memory"

#include "base/basictypes.h"

extern "C" {
#include "libavformat/avformat.h"
};

#include "decoder_base.h"
#include "ffmpeg_decoding_loop.h"
#include "ffmpeg_deleters.h"
#include "demuxer_stream.h"
#include "task_runner.h"
#include "video_frame.h"

namespace media {

class VideoDecoder : public std::enable_shared_from_this<VideoDecoder> {

 public:

  explicit VideoDecoder(TaskRunner *task_runner);

  virtual ~VideoDecoder();

  int Initialize(VideoDecodeConfig config, DemuxerStream *stream);

  using ReadCallback = std::function<void(VideoFrame)>;
  void ReadFrame(ReadCallback read_callback);

 private:

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> video_codec_context_;
  TaskRunner *decode_task_runner_ = nullptr;
  DemuxerStream *video_stream_ = nullptr;
  VideoDecodeConfig video_decode_config_;
  std::unique_ptr<FFmpegDecodingLoop> video_decoding_loop_;

  ReadCallback read_callback_;

  void VideoDecodeTask();

  bool OnNewFrameAvailable(AVFrame* frame);

  DISALLOW_COPY_AND_ASSIGN(VideoDecoder);

};

} // namespace media

#endif //MEDIA_VIDEO_DECODER_H_
