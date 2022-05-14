//
// Created by boyan on 22-5-3.
//

#ifndef LYCHEE_PLAYER__AUDIO_DECODER_H_
#define LYCHEE_PLAYER__AUDIO_DECODER_H_

#include "demuxer_stream.h"
#include "audio_buffer.h"
#include "ffmpeg_decoding_loop.h"
#include "audio_device_info.h"
#include "audio_decode_config.h"

extern "C" {
#include "libswresample/swresample.h"
}

namespace lychee {

class AudioDecoder {

 public:

  explicit AudioDecoder(const media::TaskRunner &task_runner);

  using OutputCallback = std::function<void(std::shared_ptr<AudioBuffer>)>;

  using InitializedCallback = std::function<void(bool)>;

  void Initialize(std::shared_ptr<DemuxerStream> demuxer_stream,
                  const AudioDeviceInfo &audio_device_info,
                  OutputCallback output_callback,
                  InitializedCallback initialized_callback);

  void PostDecodeTask();

 private:

  media::TaskRunner task_runner_;

  OutputCallback output_callback_;
  std::shared_ptr<DemuxerStream> stream_;

  std::unique_ptr<FFmpegDecodingLoop> ffmpeg_decoding_loop_;
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context_;

  AudioDeviceInfo audio_device_info_;
  AudioDecodeConfig audio_decode_config_;

  bool reading_;

  struct SwrContext *swr_context_;

  bool OnFrameAvailable(AVFrame *frame, int64_t serial);

  void onEndOfStream();

  void Decode(const std::shared_ptr<DecoderBuffer> &decoder_buffer);

  void DecodeTask();

  static int64 GetChannelLayout(AVFrame *av_frame);

};

}

#endif //LYCHEE_PLAYER__AUDIO_DECODER_H_
