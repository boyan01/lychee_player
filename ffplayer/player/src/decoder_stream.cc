//
// Created by yangbin on 2021/5/2.
//

#include "decoder_stream.h"

#include <utility>
#include "functional"

#include "base/bind_to_current_loop.h"
#include "base/lambda.h"

namespace media {

template<DemuxerStream::Type StreamType>
DecoderStream<StreamType>::DecoderStream(
    std::unique_ptr<DecoderStreamTraits<StreamType>> traits,
    std::shared_ptr<TaskRunner> task_runner
) : traits_(std::move(traits)), task_runner_(std::move(task_runner)),
    outputs_(), pending_decode_requests_(0),
    read_callback_(nullptr) {
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::Initialize(DemuxerStream *stream, DecoderStream::InitCallback init_callback) {

  decoder_ = std::make_unique<Decoder>();
  demuxer_stream_ = stream;

  traits_->InitializeDecoder(
      decoder_.get(), stream,
      bind_weak(&DecoderStream<StreamType>::OnFrameAvailable, this->shared_from_this()));
  auto init_callback_bound = BindToRunner(task_runner_.get(), std::move(init_callback));
  init_callback_bound(true);
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::Read(DecoderStream::ReadCallback read_callback) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  DCHECK(!read_callback_);
  read_callback_ = BindToCurrentLoop(read_callback);
  if (!outputs_.empty()) {
    read_callback_(std::move(outputs_.front()));
    outputs_.pop_front();
    read_callback_ = nullptr;
    return;
  }
  task_runner_->PostTask(FROM_HERE,
                         bind_weak(&DecoderStream<StreamType>::ReadFromDemuxerStream, this->shared_from_this()));
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::ReadFromDemuxerStream() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  if (CanDecodeMore() && !reading_demuxer_stream_) {
    reading_demuxer_stream_ = true;
    demuxer_stream_->Read(std::bind(&DecoderStream<StreamType>::OnBufferReady, this, std::placeholders::_1));
  }
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::OnBufferReady(std::shared_ptr<DecoderBuffer> buffer) {
  DCHECK(buffer);
  DCHECK(reading_demuxer_stream_);
  reading_demuxer_stream_ = false;
  task_runner_->PostTask(FROM_HERE,
                         [weak_this(std::weak_ptr<DecoderStream<StreamType>>(this->shared_from_this())), buffer]() {
                           auto ptr = weak_this.lock();
                           if (ptr) {
                             ptr->DecodeTask(buffer);
                           }
                         });
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::DecodeTask(std::shared_ptr<DecoderBuffer> decoder_buffer) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  if (decoder_buffer->end_of_stream()) {
    DLOG(WARNING) << "an end stream decode buffer";
    task_runner_->PostTask(FROM_HERE,
                           bind_weak(&DecoderStream<StreamType>::ReadFromDemuxerStream, this->shared_from_this()));
    //TODO
    return;
  }

  if (!CanDecodeMore()) {
    return;
  }

  DCHECK(demuxer_stream_);
  DCHECK(decoder_);
  DCHECK(CanDecodeMore());
  DCHECK_LT(pending_decode_requests_, GetMaxDecodeRequests());

  ++pending_decode_requests_;
  decoder_->Decode(std::move(decoder_buffer));
  --pending_decode_requests_;

  task_runner_->PostTask(FROM_HERE,
                         bind_weak(&DecoderStream<StreamType>::ReadFromDemuxerStream, this->shared_from_this()));

}

template<DemuxerStream::Type StreamType>
bool DecoderStream<StreamType>::CanDecodeMore() {
  DCHECK(task_runner_->BelongsToCurrentThread());
  return pending_decode_requests_ < GetMaxDecodeRequests() && outputs_.size() <= 9;
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::OnFrameAvailable(std::shared_ptr<Output> output) {
  DCHECK(task_runner_->BelongsToCurrentThread());

  DLOG_IF(WARNING, outputs_.size() > 9) << "outputs is full enough. " << outputs_.size();
  outputs_.emplace_back(std::move(output));
  if (read_callback_) {
    std::shared_ptr<Output> front = std::move(outputs_.front());
    outputs_.pop_front();
    read_callback_(std::move(front));
    read_callback_ = nullptr;
  }
  task_runner_->PostTask(FROM_HERE,
                         bind_weak(&DecoderStream<StreamType>::ReadFromDemuxerStream, this->shared_from_this()));
}

template<DemuxerStream::Type StreamType>
int DecoderStream<StreamType>::GetMaxDecodeRequests() {
  return 1;
}

template<DemuxerStream::Type StreamType>
void DecoderStream<StreamType>::Flush() {
  decoder_->Flush();
  outputs_.clear();
}

template
class DecoderStream<DemuxerStream::Video>;
template
class DecoderStream<DemuxerStream::Audio>;

}