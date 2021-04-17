//
// Created by yangbin on 2021/3/28.
//

#ifndef MEDIA_PLAYER_DECODER_BUFFER_H_
#define MEDIA_PLAYER_DECODER_BUFFER_H_

#include "base/buffer.h"

namespace media {

class DecoderBuffer : public Buffer {

 public:
  explicit DecoderBuffer(int size);

  ~DecoderBuffer() override;

  // Use CopyFrom instead, make Public because [CopyFrom] need.
  DecoderBuffer(const uint8 *data, int size);

  static std::shared_ptr<DecoderBuffer> CopyFrom(const uint8 *data, int size);

  static std::shared_ptr<DecoderBuffer> CreateEOSBuffer();

  const uint8 *GetData() const override;

  int GetDataSize() const override;

  /**
   * @return A read-write pointer to the buffer data.
   */
  uint8 *GetWritableData();

 private:
  int buffer_size_;
  uint8 *data_ = nullptr;

  void Initialize();

  DISALLOW_COPY_AND_ASSIGN(DecoderBuffer);

};

} // namespace media

#endif //MEDIA_PLAYER_DECODER_BUFFER_H_
