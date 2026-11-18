#include "pipeline/packet_queue.h"

namespace lychee {

void PacketQueue::SetLowWater(int64_t duration, std::function<void()> notify) {
  low_water_ = duration;
  low_water_notify_ = std::move(notify);
}

void PacketQueue::SetReplayLimits(size_t max_bytes, size_t max_packets) {
  consumed_.emplace(max_bytes, false, max_packets);
}

void PacketQueue::SetReplayAnchor(uint64_t generation, int64_t pts) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (consumed_ && generation == generation_) {
    replay_anchor_ = pts;
    consumed_->Trim(replay_anchor_);
  }
}

bool PacketQueue::Replay(int64_t pts, uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!consumed_ || consumed_->packets().empty() || pts != replay_anchor_) {
      return false;
    }
    const auto& packets = consumed_->packets();
    const EncodedPacket& start = *packets.front();
    const int64_t first_time =
        start.pts != kNoTimestamp ? start.pts : start.dts;
    if (first_time == kNoTimestamp || first_time > pts) {
      return false;
    }
    for (auto& entry : entries_) {
      entry.generation = generation;
    }
    for (auto it = packets.rbegin(); it != packets.rend(); ++it) {
      bytes_ += (*it)->size();
      entries_.push_front({*it, generation});
    }
    generation_ = generation;
    consumed_->Clear();
  }
  condition_.notify_one();
  return true;
}

void PacketQueue::Push(std::shared_ptr<EncodedPacket> packet) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_ += packet->size();
    entries_.push_back({std::move(packet), generation_});
  }
  condition_.notify_one();
}

bool PacketQueue::Pop(Entry* entry) {
  bool low = false;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    consumer_busy_ = false;
    condition_.wait(lock, [this] { return aborted_ || !entries_.empty(); });
    if (aborted_) {
      return false;
    }
    const int64_t before = low_water_notify_ ? DurationLocked() : 0;
    *entry = std::move(entries_.front());
    entries_.pop_front();
    bytes_ -= entry->packet->size();
    if (consumed_) {
      consumed_->Push(entry->packet, replay_anchor_);
    }
    consumer_busy_ = true;
    low = low_water_notify_ && before >= low_water_ &&
          DurationLocked() < low_water_;
  }
  if (low) {
    low_water_notify_();
  }
  return true;
}

void PacketQueue::Flush(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  ClearLocked(generation);
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
  ClearLocked(generation);
  aborted_ = false;
  consumer_busy_ = false;
}

void PacketQueue::ClearLocked(uint64_t generation) {
  entries_.clear();
  if (consumed_) {
    consumed_->Clear();
  }
  replay_anchor_ = kNoTimestamp;
  bytes_ = 0;
  generation_ = generation;
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
  return DurationLocked();
}

int64_t PacketQueue::DurationLocked() const {
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
