#include "pipeline/packet_queue.h"

namespace lychee {

void PacketQueue::Push(std::shared_ptr<EncodedPacket> packet) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_ += packet->size();
    entries_.push_back({std::move(packet), generation_});
  }
  condition_.notify_one();
}

bool PacketQueue::Pop(Entry* entry) {
  std::unique_lock<std::mutex> lock(mutex_);
  consumer_busy_ = false;
  condition_.wait(lock, [this] { return aborted_ || !entries_.empty(); });
  if (aborted_) {
    return false;
  }
  *entry = std::move(entries_.front());
  entries_.pop_front();
  bytes_ -= entry->packet->size();
  consumer_busy_ = true;
  return true;
}

void PacketQueue::Flush(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  bytes_ = 0;
  generation_ = generation;
}

void PacketQueue::Abort() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
  }
  condition_.notify_all();
}

void PacketQueue::Reset(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  bytes_ = 0;
  generation_ = generation;
  aborted_ = false;
  consumer_busy_ = false;
}

bool PacketQueue::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.empty();
}

bool PacketQueue::idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.empty() && !consumer_busy_;
}

size_t PacketQueue::bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bytes_;
}

int64_t PacketQueue::duration() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.empty()) {
    return 0;
  }
  int64_t first = Timestamp(*entries_.front().packet);
  int64_t last = Timestamp(*entries_.back().packet);
  if (first == kNoTimestamp || last == kNoTimestamp || last < first) {
    return 0;
  }
  return last - first + entries_.back().packet->duration;
}

// static
int64_t PacketQueue::Timestamp(const EncodedPacket& packet) {
  return packet.dts != kNoTimestamp ? packet.dts : packet.pts;
}

}  // namespace lychee
