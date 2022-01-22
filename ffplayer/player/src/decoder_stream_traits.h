//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_DECODER_STREAM_TRAITS_H_
#define MEDIA_PLAYER_SRC_DECODER_STREAM_TRAITS_H_

#include "audio_decoder.h"
#include "demuxer_stream.h"
#include "video_decoder.h"

namespace media {

template <DemuxerStream::Type StreamType>
class DecoderStreamTraits {};

template <>
class DecoderStreamTraits<DemuxerStream::Video> {
 public:
  using OutputType = VideoFrame;
  using DecoderType = VideoDecoder;
  using DecoderConfigType = VideoDecodeConfig;

  using OutputCallback = VideoDecoder::OutputCallback;

  ~DecoderStreamTraits();

  void InitializeDecoder(DecoderType *decoder, DemuxerStream *stream,
                         OutputCallback output_callback);
};

template <>
class DecoderStreamTraits<DemuxerStream::Audio> {
 public:
  using OutputType = AudioBuffer;
  using DecoderType = AudioDecoder;
  using DecoderConfigType = AudioDecodeConfig;

  using OutputCallback = AudioDecoder::OutputCallback;

  ~DecoderStreamTraits();

  void InitializeDecoder(DecoderType *decoder, DemuxerStream *stream,
                         OutputCallback output_callback);
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_DECODER_STREAM_TRAITS_H_
