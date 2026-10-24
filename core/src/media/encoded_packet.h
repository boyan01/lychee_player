#ifndef LYCHEE_MEDIA_ENCODED_PACKET_H_
#define LYCHEE_MEDIA_ENCODED_PACKET_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/time.h"

namespace lychee {

// A compressed sample produced by a Demuxer. Timestamps are in microseconds
// on the media timeline.
struct EncodedPacket {
  int stream_index = -1;
  int64_t pts = kNoTimestamp;
  int64_t dts = kNoTimestamp;
  int64_t duration = 0;
  bool keyframe = false;
  bool end_of_stream = false;
  std::vector<uint8_t> data;

  static std::shared_ptr<EncodedPacket> CreateEndOfStream(int stream_index) {
    auto packet = std::make_shared<EncodedPacket>();
    packet->stream_index = stream_index;
    packet->end_of_stream = true;
    return packet;
  }
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_ENCODED_PACKET_H_
