#include "pipeline/packet_queue.h"

#include <thread>

#include "test.h"

namespace lychee {
namespace {

std::shared_ptr<EncodedPacket> MakePacket(int64_t dts, size_t size) {
  auto packet = std::make_shared<EncodedPacket>();
  packet->dts = packet->pts = dts;
  packet->duration = 1000;
  std::vector<uint8_t> bytes(size, 0xab);
  packet->buffer = PacketBuffer::Copy(bytes.data(), bytes.size());
  return packet;
}

TEST(PacketQueue, PopsInOrderAndTracksSize) {
  PacketQueue queue;
  queue.Push(MakePacket(0, 10));
  queue.Push(MakePacket(1000, 20));
  queue.Push(MakePacket(2000, 30));
  EXPECT_EQ(queue.bytes(), 60u);
  EXPECT_EQ(queue.duration(), 3000);

  PacketQueue::Entry entry;
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_EQ(entry.packet->dts, 0);
  EXPECT_EQ(queue.bytes(), 50u);
  EXPECT_EQ(queue.duration(), 2000);
}

// Test registration allocates before main, as the existing TESTs do.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(PacketQueue, NotifiesWhenAPopCrossesTheLowWater) {
  PacketQueue queue;
  int notified = 0;
  queue.SetLowWater(2500, [&notified] { notified++; });
  for (int64_t dts = 0; dts < 5000; dts += 1000) {
    queue.Push(MakePacket(dts, 10));
  }
  PacketQueue::Entry entry;
  // 5000 -> 4000 -> 3000: still above.
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_EQ(notified, 0);
  // -> 2000: crossed, once.
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_EQ(notified, 1);
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_EQ(notified, 1);
  // Refilled above it, then drained below again.
  for (int64_t dts = 5000; dts < 8000; dts += 1000) {
    queue.Push(MakePacket(dts, 10));
  }
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_EQ(notified, 2);
}

TEST(PacketQueue, PaddingIsZero) {
  auto packet = MakePacket(0, 5);
  const uint8_t* end = packet->buffer->data() + packet->size();
  bool zero = true;
  for (size_t i = 0; i < kPacketPadding; ++i) {
    zero = zero && end[i] == 0;
  }
  EXPECT_TRUE(zero);
}

TEST(PacketQueue, FlushStartsNewGeneration) {
  PacketQueue queue;
  queue.Push(MakePacket(0, 10));
  queue.Flush(7);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.bytes(), 0u);
  queue.Push(MakePacket(5000, 10));
  PacketQueue::Entry entry;
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_EQ(entry.generation, 7u);
  EXPECT_EQ(entry.packet->dts, 5000);
}

TEST(PacketQueue, EndOfStreamHasNoBytes) {
  PacketQueue queue;
  queue.Push(EncodedPacket::CreateEndOfStream(0));
  EXPECT_EQ(queue.bytes(), 0u);
  PacketQueue::Entry entry;
  EXPECT_TRUE(queue.Pop(&entry));
  EXPECT_TRUE(entry.packet->end_of_stream);
}

TEST(PacketQueue, AbortWakesBlockedPop) {
  PacketQueue queue;
  bool popped = true;
  std::thread consumer([&] {
    PacketQueue::Entry entry;
    popped = queue.Pop(&entry);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.Abort();
  consumer.join();
  EXPECT_TRUE(!popped);
}

TEST(PacketQueue, PopWaitsForPush) {
  PacketQueue queue;
  int64_t dts = -1;
  std::thread consumer([&] {
    PacketQueue::Entry entry;
    if (queue.Pop(&entry)) {
      dts = entry.packet->dts;
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.Push(MakePacket(42, 1));
  consumer.join();
  EXPECT_EQ(dts, 42);
}

}  // namespace
}  // namespace lychee
