//
// Created by yangbin on 2021/5/9.
//

#ifndef MEDIA_PLAYER_SRC_DECODER_BUFFER_H_
#define MEDIA_PLAYER_SRC_DECODER_BUFFER_H_

#include <base/basictypes.h>

#include "ffmpeg_deleters.h"

namespace lychee {

class DecoderBuffer {
 public:
  explicit DecoderBuffer(std::unique_ptr<AVPacket, AVPacketDeleter> av_packet, int64_t serial);

  static std::shared_ptr<DecoderBuffer> CreateEOSBuffer(int64_t serial);

  size_t data_size();

  [[nodiscard]] double timestamp() const { return timestamp_; }

  void set_timestamp(double timestamp) { timestamp_ = timestamp; }

  AVPacket *av_packet() { return av_packet_.get(); }

  bool end_of_stream();
  virtual ~DecoderBuffer();

  [[nodiscard]] int64_t serial() const { return serial_; }

 private:
  std::unique_ptr<AVPacket, AVPacketDeleter> av_packet_;

  // pts.
  double timestamp_;

  int64_t serial_ = 0;

  DELETE_COPY_AND_ASSIGN(DecoderBuffer);
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_DECODER_BUFFER_H_
