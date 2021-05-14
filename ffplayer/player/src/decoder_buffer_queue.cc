//
// Created by yangbin on 2021/5/9.
//

#include "decoder_buffer_queue.h"

#include "base/logging.h"

namespace media {

DecoderBufferQueue::DecoderBufferQueue() : data_size_(0), earliest_valid_timestamp_(-1) {

}

DecoderBufferQueue::~DecoderBufferQueue() = default;

void DecoderBufferQueue::Push(std::shared_ptr<DecoderBuffer> buffer) {
  DCHECK(!buffer->end_of_stream());

  queue_.push_back(buffer);
  data_size_ += buffer->data_size();

  if (buffer->timestamp() < 0) {
    DLOG(WARNING) << "Buffer has no timestamp: " << buffer->timestamp();
    return;
  }

  if (earliest_valid_timestamp_ < 0) {
    earliest_valid_timestamp_ = buffer->timestamp();
  }

  if (buffer->timestamp() < earliest_valid_timestamp_) {
//    DLOG(WARNING) << "Out of order timestamps: "
//                  << buffer->timestamp() << " vs. "
//                  << earliest_valid_timestamp_;
    return;
  }

  earliest_valid_timestamp_ = buffer->timestamp();
  in_order_queue_.emplace_back(std::move(buffer));

}

std::shared_ptr<DecoderBuffer> DecoderBufferQueue::Pop() {
  auto buffer = std::move(queue_.front());
  queue_.pop_front();

  auto buffer_data_size = buffer->data_size();
  DCHECK_LE(buffer_data_size, data_size_);
  data_size_ -= buffer_data_size;

  if (!in_order_queue_.empty() && in_order_queue_.front() == buffer) {
    in_order_queue_.pop_front();
  }

  return buffer;
}

void DecoderBufferQueue::Clear() {
  data_size_ = 0;
  earliest_valid_timestamp_ = -1;
  queue_.clear();
  in_order_queue_.clear();
}

bool DecoderBufferQueue::IsEmpty() {
  return queue_.empty();
}

double DecoderBufferQueue::Duration() {
  if (in_order_queue_.size() < 2) {
    return 0;
  }
  auto start = in_order_queue_.front()->timestamp();
  auto end = in_order_queue_.back()->timestamp();
  return end - start;
}

}