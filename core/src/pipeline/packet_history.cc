#include "pipeline/packet_history.h"

#include <algorithm>

namespace lychee {

PacketHistory::PacketHistory(size_t max_bytes, bool every_packet_starts)
    : max_bytes_(max_bytes), every_packet_starts_(every_packet_starts) {}

void PacketHistory::Push(std::shared_ptr<EncodedPacket> packet,
                         int64_t position) {
  if (packets_.empty() && !Starts(*packet)) {
    return;
  }
  if (!every_packet_starts_ && packet->keyframe) {
    starts_.push_back(front_number_ + packets_.size());
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
  starts_.clear();
  bytes_ = 0;
  front_number_ = 0;
}

bool PacketHistory::Starts(const EncodedPacket& packet) const {
  return every_packet_starts_ || packet.keyframe;
}

size_t PacketHistory::NextStart() const {
  if (every_packet_starts_) {
    return std::min<size_t>(1, packets_.size());
  }
  // The oldest packet is a keyframe: the one after it is the next.
  if (starts_.size() < 2) {
    return packets_.size();
  }
  return static_cast<size_t>(starts_[1] - front_number_);
}

void PacketHistory::DropFront(size_t count) {
  for (size_t i = 0; i < count && !packets_.empty(); ++i) {
    bytes_ -= packets_.front()->size();
    packets_.pop_front();
    front_number_++;
  }
  while (!starts_.empty() && starts_.front() < front_number_) {
    starts_.pop_front();
  }
}

}  // namespace lychee
