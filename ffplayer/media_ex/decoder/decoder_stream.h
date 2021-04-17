//
// Created by yangbin on 2021/4/8.
//

#ifndef MEDIA_PLAYER_DECODER_DECODER_STREAM_H_
#define MEDIA_PLAYER_DECODER_DECODER_STREAM_H_

#include <utility>

#include "memory"

#include "base/message_loop.h"

#include "demuxer/demuxer_stream.h"
#include "decoder/decoder_stream_traits.h"

namespace media {

template<DemuxerStream::Type StreamType>
class DecoderStream {
 public:
  using StreamTraits = DecoderStreamTraits<StreamType>;
  using Decoder = typename StreamTraits::DecoderType;
  using Output = typename StreamTraits::OutputType;
  using DecoderConfig = typename StreamTraits::DecoderConfigType;

  enum ReadStatus {
    OK,
    ABORTED,
    DEMUXER_READ_ABORT,
    DECODE_ERROR,
  };

  DecoderStream(std::unique_ptr<DecoderStreamTraits<StreamType>> traits,
                std::shared_ptr<base::MessageLoop> task_runner);

  ~DecoderStream();

 private:

  enum State {
    STATE_UNINITIALIZED,
    STATE_INITIALIZING,
    STATE_NORMAL,  // Includes idle, pending decoder decode/reset.
    STATE_FLUSHING_DECODER,
    STATE_REINITIALIZING_DECODER,
    STATE_END_OF_STREAM,  // End of stream reached; returns EOS on all reads.
    STATE_ERROR,
  };

  std::unique_ptr<DecoderStreamTraits<StreamType>> traits_;
  std::shared_ptr<base::MessageLoop> task_runner_;

  State state_;

  DemuxerStream *stream_ = nullptr;

};

using VideoDecoderStream = DecoderStream<DemuxerStream::VIDEO>;
using AudioDecoderStream = DecoderStream<DemuxerStream::AUDIO>;

} // namespace media

#endif //MEDIA_PLAYER_DECODER_DECODER_STREAM_H_
