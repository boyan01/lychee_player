//
// Created by yangbin on 2021/5/2.
//

#include "audio_buffer.h"

#include "base/logging.h"
#include "cstring"

namespace media {

#define MAX_AUDIO_VOLUME 100
#define ADJUST_VOLUME(s, v) (s = (s * v) / MAX_AUDIO_VOLUME)

// from SDL_MixAudioFormat#AUDIO_S16LSB
static void MixAudioVolume(uint8 *dst, const uint8 *src, uint32_t len,
                           int volume) {
  int16_t src1, src2;
  int dst_sample;
  const int max_audioval = ((1 << (16 - 1)) - 1);
  const int min_audioval = -(1 << (16 - 1));

  len /= 2;
  while (len--) {
    src1 = ((src[1]) << 8 | src[0]);
    ADJUST_VOLUME(src1, volume);
    src2 = ((dst[1]) << 8 | dst[0]);
    src += 2;
    dst_sample = src1 + src2;
    if (dst_sample > max_audioval) {
      dst_sample = max_audioval;
    } else if (dst_sample < min_audioval) {
      dst_sample = min_audioval;
    }
    dst[0] = dst_sample & 0xFF;
    dst_sample >>= 8;
    dst[1] = dst_sample & 0xFF;
    dst += 2;
  }
}

AudioBuffer::AudioBuffer(uint8 *data, int size, double pts, int bytes_per_sec)
    : data_(data), size_(size), pts_(pts), bytes_per_sec_(bytes_per_sec) {}

AudioBuffer::~AudioBuffer() { free(data_); }

int AudioBuffer::Read(uint8 *dest, int size, double volume) {
  DCHECK_GT(size, 0);
  DCHECK_LT(read_cursor_, size_);

  auto flush_size = std::min(size, size_ - read_cursor_);

  if (volume == 0) {
    memset(dest, 0, flush_size);
  } else if (volume == 1) {
    memcpy(dest, data_ + read_cursor_, flush_size);
  } else {
    memset(dest, 0, flush_size);
    MixAudioVolume(dest, data_ + read_cursor_, flush_size, int(volume * 100));
  }

  read_cursor_ += flush_size;
  return flush_size;
}

}  // namespace media