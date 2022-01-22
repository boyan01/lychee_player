//
// Created by yangbin on 2021/5/2.
//

#ifndef MEDIA_PLAYER_SRC_DECODER_STREAM_H_
#define MEDIA_PLAYER_SRC_DECODER_STREAM_H_

#include <ostream>

#include "base/basictypes.h"
#include "base/circular_deque.h"
#include "decoder_stream_traits.h"
#include "demuxer_stream.h"
#include "functional"
#include "memory"

namespace media {

template <DemuxerStream::Type StreamType>
class DecoderStream
    : public std::enable_shared_from_this<DecoderStream<StreamType>> {
 public:
  using StreamTraits = DecoderStreamTraits<StreamType>;
  using Decoder = typename StreamTraits::DecoderType;
  using Output = typename StreamTraits::OutputType;
  using DecoderConfig = typename StreamTraits::DecoderConfigType;

  using ReadResult = std::shared_ptr<Output>;
  using ReadCallback = std::function<void(ReadResult)>;

  explicit DecoderStream(
      std::unique_ptr<DecoderStreamTraits<StreamType>> traits,
      std::shared_ptr<TaskRunner> task_runner);

  void Read(ReadCallback read_callback);

  using InitCallback = std::function<void(bool success)>;
  void Initialize(DemuxerStream *stream, InitCallback init_callback);

  void Flush();

  friend std::ostream &operator<<(std::ostream &os,
                                  const DecoderStream<StreamType> &stream) {
    os << " outputs_: " << stream.outputs_.size()
       << " pending_decode_requests_: " << stream.pending_decode_requests_
       << " reading_demuxer_stream_: " << stream.reading_demuxer_stream_
       << " read_callback_: " << (stream.read_callback_ != nullptr);
    return os;
  }

 private:
  std::unique_ptr<Decoder> decoder_;
  std::unique_ptr<StreamTraits> traits_;

  DemuxerStream *demuxer_stream_ = nullptr;

  std::deque<std::shared_ptr<Output>> outputs_;

  ReadCallback read_callback_;

  std::shared_ptr<TaskRunner> task_runner_;

  int pending_decode_requests_;

  bool reading_demuxer_stream_ = false;

  void ReadFromDemuxerStream();

  void OnBufferReady(std::shared_ptr<DecoderBuffer> buffer);

  void DecodeTask(std::shared_ptr<DecoderBuffer> decoder_buffer);

  void OnFrameAvailable(std::shared_ptr<Output> output);

  int GetMaxDecodeRequests();

  bool CanDecodeMore();

  DELETE_COPY_AND_ASSIGN(DecoderStream);
};

using AudioDecoderStream = DecoderStream<DemuxerStream::Audio>;
using VideoDecoderStream = DecoderStream<DemuxerStream::Video>;

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_DECODER_STREAM_H_
