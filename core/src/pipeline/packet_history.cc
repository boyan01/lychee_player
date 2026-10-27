#include "pipeline/packet_history.h"

namespace lychee {

PacketHistory::PacketHistory(size_t max_bytes, bool every_packet_starts)
    : max_bytes_(max_bytes), every_packet_starts_(every_packet_starts) {}

void PacketHistory::Push(std::shared_ptr<EncodedPacket> packet,
                         int64_t position) {
  if (packets_.empty() && !Starts(*packet)) {
    return;
  }
  bytes_ += packet->size();
  packets_.push_back(std::move(packet));
  if (position != kNoTimestamp) {
    for (size_t next = NextStart(); next < packets_.size();
         next = NextStart()) {
      const EncodedPacket& start = *packets_[next];
      const int64_t time = start.pts != kNoTimestamp ? start.pts : start.dts;
      if (time == kNoTimestamp || time > position) {
        break;
      }
      DropFront(next);
    }
  }
  while (bytes_ > max_bytes_) {
    DropFront(NextStart());
  }
}

void PacketHistory::Clear() {
  packets_.clear();
  bytes_ = 0;
}

bool PacketHistory::Starts(const EncodedPacket& packet) const {
  return every_packet_starts_ || packet.keyframe;
}

size_t PacketHistory::NextStart() const {
  size_t next = 1;
  while (next < packets_.size() && !Starts(*packets_[next])) {
    next++;
  }
  return next;
}

void PacketHistory::DropFront(size_t count) {
  for (size_t i = 0; i < count && !packets_.empty(); ++i) {
    bytes_ -= packets_.front()->size();
    packets_.pop_front();
  }
}

}  // namespace lychee
