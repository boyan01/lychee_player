//
// Created by yangbin on 2021/5/9.
//

#include "decoder_buffer.h"

#include "base/logging.h"

namespace media {

// static
std::shared_ptr<DecoderBuffer> DecoderBuffer::CreateEOSBuffer() {
  return std::make_shared<DecoderBuffer>(nullptr);
}

DecoderBuffer::DecoderBuffer(std::unique_ptr<AVPacket, AVPacketDeleter> av_packet)
    : timestamp_(-1) {
  if (av_packet) {
    av_packet_ = new AVPacket;
    *av_packet_ = *av_packet;
    av_packet_ref(av_packet_, av_packet.get());
  } else {
    av_packet_ = nullptr;
  }
}

DecoderBuffer::~DecoderBuffer() {
  if (av_packet_) {
    av_packet_unref(av_packet_);
    delete av_packet_;
  }
}

size_t DecoderBuffer::data_size() {
  DCHECK(!end_of_stream());
  return av_packet_->size;
}

bool DecoderBuffer::end_of_stream() {
  return av_packet_ == nullptr;
}

}