//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_DECODER_STREAM_TRAITS_H_
#define MEDIA_PLAYER_SRC_DECODER_STREAM_TRAITS_H_

#include "audio_decoder.h"
#include "video_decoder.h"

#include "demuxer_stream.h"

namespace media {

template<DemuxerStream::Type StreamType>
class DecoderStreamTraits {

};

template<>
class DecoderStreamTraits<DemuxerStream::Video> {
 public:
  using OutputType = VideoFrame;
  using DecoderType = VideoDecoder2;
  using DecoderConfigType = VideoDecodeConfig;

  using OutputCallback = VideoDecoder2::OutputCallback;

  ~DecoderStreamTraits();

  void InitializeDecoder(DecoderType *decoder, DemuxerStream *stream, OutputCallback output_callback);

};

template<>
class DecoderStreamTraits<DemuxerStream::Audio> {
 public:
  using OutputType = AudioBuffer;
  using DecoderType = AudioDecoder2;
  using DecoderConfigType = AudioDecodeConfig;

  using OutputCallback = AudioDecoder2::OutputCallback;

  ~DecoderStreamTraits();

  void InitializeDecoder(DecoderType *decoder, DemuxerStream *stream, OutputCallback output_callback);

};

}

#endif //MEDIA_PLAYER_SRC_DECODER_STREAM_TRAITS_H_
