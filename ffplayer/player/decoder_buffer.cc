//
// Created by yangbin on 2021/3/28.
//

#include "logging.h"
#include "decoder_buffer.h"

namespace media {

DecoderBuffer::DecoderBuffer(int size)
    : Buffer(chrono::milliseconds::zero(), chrono::milliseconds::zero()),
      buffer_size_(size) {
  Initialize();
}

DecoderBuffer::DecoderBuffer(const uint8 *data, int size)
    : Buffer(chrono::milliseconds::zero(), chrono::milliseconds::zero()), buffer_size_(size) {
  if (!data) {
    buffer_size_ = 0;
    data_ = nullptr;
    return;
  }
  Initialize();
  memcpy(data_, data, buffer_size_);
}

DecoderBuffer::~DecoderBuffer() {
  delete[] data_;
}

void DecoderBuffer::Initialize() {
  DCHECK_GE(buffer_size_, 0);
  data_ = new uint8[buffer_size_];
}

std::shared_ptr<DecoderBuffer> DecoderBuffer::CopyFrom(const uint8 *data, int size) {
  DCHECK(data);
  return std::make_shared<DecoderBuffer>(data, size);
}

std::shared_ptr<DecoderBuffer> DecoderBuffer::CreateEOSBuffer() {
  return std::make_shared<DecoderBuffer>(nullptr, 0);
}

const uint8 *DecoderBuffer::GetData() const {
  return data_;
}

int DecoderBuffer::GetDataSize() const {
  return buffer_size_;
}

uint8 *DecoderBuffer::GetWritableData() {
  return data_;
}

} // namespace media
