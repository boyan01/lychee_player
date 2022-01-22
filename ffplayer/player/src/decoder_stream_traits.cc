//
// Created by yangbin on 2021/5/2.
//

#include "decoder_stream_traits.h"

namespace media {

DecoderStreamTraits<DemuxerStream::Video>::~DecoderStreamTraits() {}

void DecoderStreamTraits<DemuxerStream::Video>::InitializeDecoder(
    VideoDecoder *decoder, DemuxerStream *stream,
    OutputCallback output_callback) {
  decoder->Initialize(stream->video_decode_config(), stream,
                      std::move(output_callback));
}

DecoderStreamTraits<DemuxerStream::Audio>::~DecoderStreamTraits() {}

void DecoderStreamTraits<DemuxerStream::Audio>::InitializeDecoder(
    DecoderType *decoder, DemuxerStream *stream,
    OutputCallback output_callback) {
  DCHECK(decoder);
  DCHECK(stream);
  decoder->Initialize(stream->audio_decode_config(), stream,
                      std::move(output_callback));
}

}  // namespace media