//
// Created by yangbin on 2021/5/9.
//

#ifndef MEDIA_PLAYER_SRC_DECODER_BUFFER_QUEUE_H_
#define MEDIA_PLAYER_SRC_DECODER_BUFFER_QUEUE_H_

#include "decoder_buffer.h"
#include "queue"

namespace lychee {

class DecoderBufferQueue {
 public:
  DELETE_COPY_AND_ASSIGN(DecoderBufferQueue);

  DecoderBufferQueue();

  ~DecoderBufferQueue();

  void Push(std::shared_ptr<DecoderBuffer> buffer);

  std::shared_ptr<DecoderBuffer> Pop();

  void Clear();

  bool IsEmpty();

  double Duration();

  [[nodiscard]] size_t data_size() const { return data_size_; }

 private:
  std::deque<std::shared_ptr<DecoderBuffer>> queue_;
  std::deque<std::shared_ptr<DecoderBuffer>> in_order_queue_;

  size_t data_size_;

  double earliest_valid_timestamp_;
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_DECODER_BUFFER_QUEUE_H_
