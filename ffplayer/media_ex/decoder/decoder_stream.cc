//
// Created by yangbin on 2021/4/8.
//

#include "decoder/decoder_stream.h"

#include "base/logging.h"

namespace media {

template<DemuxerStream::Type StreamType>
DecoderStream<StreamType>::DecoderStream(
    std::unique_ptr<DecoderStreamTraits<StreamType>> traits,
    std::shared_ptr<base::MessageLoop> task_runner
): traits_(std::move(traits)), task_runner_(std::move(task_runner)) {

}
template<DemuxerStream::Type StreamType>
DecoderStream<StreamType>::~DecoderStream() {
  DCHECK(task_runner_->BelongsToCurrentThread());
}

} // namespace media