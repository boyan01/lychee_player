//
// Created by yangbin on 2021/5/2.
//

#include "audio_buffer.h"

#include "base/logging.h"

namespace media {


AudioBuffer::AudioBuffer(uint8 *data, int size, double pts, int bytes_per_sec)
    : data_(data), size_(size), pts_(pts), bytes_per_sec_(bytes_per_sec) {}

AudioBuffer::~AudioBuffer() {

}

int AudioBuffer::Read(uint8 *dest, int size) {
  DCHECK(size > 0);

  auto flush_size = std::min(size, size_ - read_cursor_);
  memcpy(dest, data_ + read_cursor_, flush_size);

  read_cursor_ += flush_size;
  return flush_size;
}



}