//
// Created by yangbin on 2021/5/9.
//

#include "decoder_buffer.h"

#include "base/logging.h"

namespace media {

// static
std::shared_ptr<DecoderBuffer> DecoderBuffer::CreateEOSBuffer() {
  return std::shared_ptr<DecoderBuffer>(nullptr);
}

DecoderBuffer::DecoderBuffer(std::unique_ptr<AVPacket, AVPacketDeleter> av_packet)
    : av_packet_(std::move(av_packet)),
      timestamp_(-1) {

}

size_t DecoderBuffer::data_size() {
  DCHECK(!end_of_stream());
  return av_packet_->size;
}

bool DecoderBuffer::end_of_stream() {
  return av_packet_ == nullptr;
}

}