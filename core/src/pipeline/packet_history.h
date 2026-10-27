#ifndef LYCHEE_PIPELINE_PACKET_HISTORY_H_
#define LYCHEE_PIPELINE_PACKET_HISTORY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>

#include "media/encoded_packet.h"

namespace lychee {

// The packets of one stream read since the last keyframe at or before the
// playback position. On input that cannot seek, a resync replays them so
// new decoders start where playback is rather than at the read position
// (Player::ProcessSeek()). The packets are shared with the packet queues,
// so only those already decoded cost memory.
class PacketHistory {
 public:
  // |every_packet_starts|: a decoder can start at any packet (audio).
  // Past |max_bytes| the oldest packets go, up to the next keyframe.
  PacketHistory(size_t max_bytes, bool every_packet_starts);

  // Adds |packet| and drops what a decoder starting at |position| does not
  // need: the packets before the last keyframe at or before it. Packets
  // before the first keyframe are not kept.
  void Push(std::shared_ptr<EncodedPacket> packet, int64_t position);
  void Clear();

  // Oldest first, starting with a keyframe.
  const std::deque<std::shared_ptr<EncodedPacket>>& packets() const {
    return packets_;
  }
  size_t bytes() const { return bytes_; }

 private:
  bool Starts(const EncodedPacket& packet) const;
  // Index of the first keyframe after the oldest packet, or size().
  size_t NextStart() const;
  void DropFront(size_t count);

  const size_t max_bytes_;
  const bool every_packet_starts_;
  std::deque<std::shared_ptr<EncodedPacket>> packets_;
  size_t bytes_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_PACKET_HISTORY_H_
