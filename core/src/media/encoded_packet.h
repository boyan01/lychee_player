#ifndef LYCHEE_MEDIA_ENCODED_PACKET_H_
#define LYCHEE_MEDIA_ENCODED_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "base/time.h"

namespace lychee {

// Zero bytes after the end of every packet's data: decoders (FFmpeg's among
// them) may read a little past the end of a bitstream.
inline constexpr size_t kPacketPadding = 64;

// The bytes of one packet, shared by reference from the demuxer to the
// decoder and immutable once created. |data| is followed by kPacketPadding
// zero bytes. Backends subclass it to hand over their own buffers.
class PacketBuffer {
 public:
  virtual ~PacketBuffer() = default;

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

  static std::shared_ptr<PacketBuffer> Copy(const uint8_t* data, size_t size);

 protected:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// A compressed sample produced by a Demuxer. Timestamps are in microseconds
// on the media timeline.
struct EncodedPacket {
  int stream_index = -1;
  int64_t pts = kNoTimestamp;
  int64_t dts = kNoTimestamp;
  int64_t duration = 0;
  bool keyframe = false;
  bool end_of_stream = false;
  // Not data: the stream starts over at |pts| (a loop). The packets before
  // play up to |pts| and no further; the decoder drains and is flushed.
  bool boundary = false;
  // Null for end of stream and boundaries.
  std::shared_ptr<const PacketBuffer> buffer;

  size_t size() const { return buffer ? buffer->size() : 0; }

  static std::shared_ptr<EncodedPacket> CreateEndOfStream(int stream_index) {
    auto packet = std::make_shared<EncodedPacket>();
    packet->stream_index = stream_index;
    packet->end_of_stream = true;
    return packet;
  }

  static std::shared_ptr<EncodedPacket> CreateBoundary(int stream_index,
                                                       int64_t time) {
    auto packet = std::make_shared<EncodedPacket>();
    packet->stream_index = stream_index;
    packet->pts = time;
    packet->boundary = true;
    return packet;
  }
};

inline std::shared_ptr<PacketBuffer> PacketBuffer::Copy(const uint8_t* data,
                                                        size_t size) {
  class Owned : public PacketBuffer {
   public:
    Owned(const uint8_t* data, size_t size) : bytes_(size + kPacketPadding) {
      if (size > 0) {
        memcpy(bytes_.data(), data, size);
      }
      data_ = bytes_.data();
      size_ = size;
    }

   private:
    std::vector<uint8_t> bytes_;
  };
  return std::make_shared<Owned>(data, size);
}

}  // namespace lychee

#endif  // LYCHEE_MEDIA_ENCODED_PACKET_H_
