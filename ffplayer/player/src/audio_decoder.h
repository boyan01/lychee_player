//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_AUDIO_DEOCDER_H_
#define MEDIA_PLAYER_SRC_AUDIO_DEOCDER_H_

#include "memory"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
}

#include "base/circular_deque.h"

#include "task_runner.h"
#include "audio_decode_config.h"
#include "demuxer_stream.h"
#include "audio_buffer.h"
#include "ffmpeg_deleters.h"
#include "ffmpeg_decoding_loop.h"
#include "audio_device_info.h"

namespace media {

class AudioDecoder {

 public:

  explicit AudioDecoder();

  virtual ~AudioDecoder();

  using OutputCallback = std::function<void(std::shared_ptr<AudioBuffer>)>;

  int Initialize(const AudioDecodeConfig &config, DemuxerStream *stream, OutputCallback output_callback);

  void Decode(std::shared_ptr<DecoderBuffer> decoder_buffer);

 private:

  AudioDecodeConfig audio_decode_config_;
  std::unique_ptr<FFmpegDecodingLoop> ffmpeg_decoding_loop_;

  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context_;

  DemuxerStream *stream_ = nullptr;

  OutputCallback output_callback_;

  struct SwrContext *swr_ctx_ = nullptr;

  AudioDeviceInfo audio_device_info_;

  bool OnFrameAvailable(AVFrame *frame);

  static int64 GetChannelLayout(AVFrame *frame);

  DISALLOW_COPY_AND_ASSIGN(AudioDecoder);

};

}
#endif //MEDIA_PLAYER_SRC_AUDIO_DEOCDER_H_
