#ifndef LYCHEE_PIPELINE_PACKET_QUEUE_H_
#define LYCHEE_PIPELINE_PACKET_QUEUE_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "media/encoded_packet.h"

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

  void Push(std::shared_ptr<EncodedPacket> packet);

  // Blocks until a packet is available. Returns false once aborted.
  bool Pop(Entry* entry);

  // Drops queued packets; later pushes belong to |generation|.
  void Flush(uint64_t generation);

  void Abort();
  // Empties the queue and undoes Abort(), for a new consumer. Later pushes
  // belong to |generation|.
  void Reset(uint64_t generation);

  bool empty() const;
  size_t bytes() const;
  // Span of the queued packets' timestamps.
  int64_t duration() const;

 private:
  static int64_t Timestamp(const EncodedPacket& packet);

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Entry> entries_;
  uint64_t generation_ = 0;
  size_t bytes_ = 0;
  bool aborted_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_PACKET_QUEUE_H_
