#ifndef LYCHEE_PIPELINE_PACKET_QUEUE_H_
#define LYCHEE_PIPELINE_PACKET_QUEUE_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "media/encoded_packet.h"
#include "pipeline/packet_history.h"

namespace lychee {

// Demuxer -> decoder hand-off. Every packet carries the generation it was
// queued in; a seek bumps the generation so consumers can tell stale data
// from fresh data and flush their decoders.
class PacketQueue {
 public:
  struct Entry {
    std::shared_ptr<EncodedPacket> packet;
    uint64_t generation = 0;
  };

  // Before any other call: |notify| runs when a Pop() takes duration()
  // below |duration|, on the popping thread, holding none of the queue's
  // locks, so the producer can wait for that rather than poll.
  void SetLowWater(int64_t duration, std::function<void()> notify);

  // Before any Push or Pop: retain consumed video packets for replay from
  // the last frame presented, with independent byte and packet limits.
  void SetReplayLimits(size_t max_bytes, size_t max_packets);
  void SetReplayAnchor(uint64_t generation, int64_t pts);
  // Prepends the consumed packets needed to show |pts| again, keeping the
  // unread packets after them. False if the GOP no longer fits the cache.
  bool Replay(int64_t pts, uint64_t generation);

  void Push(std::shared_ptr<EncodedPacket> packet);

  // Blocks until a packet is available. Returns false once aborted. The
  // consumer counts as busy with the packet until it calls Pop() again.
  bool Pop(Entry* entry);

  // Drops queued packets; later pushes belong to |generation|.
  void Flush(uint64_t generation);

  void Abort();
  // Empties the queue and undoes Abort(), for a new consumer. Later pushes
  // belong to |generation|.
  void Reset(uint64_t generation);

  bool empty() const;
  // Empty, and the consumer is done with the last packet: waiting in Pop().
  bool idle() const;
  size_t bytes() const;
  // Span of the queued packets' timestamps.
  int64_t duration() const;

 private:
  static int64_t Timestamp(const EncodedPacket& packet);
  int64_t DurationLocked() const;
  void ClearLocked(uint64_t generation);

  int64_t low_water_ = 0;
  std::function<void()> low_water_notify_;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Entry> entries_;
  std::optional<PacketHistory> consumed_;
  int64_t replay_anchor_ = kNoTimestamp;
  uint64_t generation_ = 0;
  size_t bytes_ = 0;
  bool aborted_ = false;
  bool consumer_busy_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_PACKET_QUEUE_H_
