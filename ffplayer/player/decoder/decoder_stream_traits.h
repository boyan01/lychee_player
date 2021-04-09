//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_PLAYER_DECODER_DECODER_STREAM_TRAITS_H_
#define MEDIA_PLAYER_DECODER_DECODER_STREAM_TRAITS_H_

#include "demuxer/demuxer_stream.h"

namespace media {

template<DemuxerStream::Type StreamType>
class DecoderStreamTraits {};


} // namespace media

#endif //MEDIA_PLAYER_DECODER_DECODER_STREAM_TRAITS_H_
