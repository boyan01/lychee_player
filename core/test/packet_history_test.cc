#include "pipeline/packet_history.h"

#include <vector>

#include "test.h"

namespace lychee {
namespace {

std::shared_ptr<EncodedPacket> MakePacket(int64_t pts,
                                          bool keyframe,
                                          size_t size = 10) {
  auto packet = std::make_shared<EncodedPacket>();
  packet->pts = packet->dts = pts;
  packet->keyframe = keyframe;
  std::vector<uint8_t> bytes(size, 0xab);
  packet->buffer = PacketBuffer::Copy(bytes.data(), bytes.size());
  return packet;
}

std::vector<int64_t> Times(const PacketHistory& history) {
  std::vector<int64_t> times;
  for (const auto& packet : history.packets()) {
    times.push_back(packet->pts);
  }
  return times;
}

TEST(PacketHistory, KeepsFromTheKeyframeAtOrBeforeThePosition) {
  PacketHistory history(1 << 20, false);
  // Keyframes every 3 packets: 0, 3, 6.
  for (int64_t pts = 0; pts < 9; ++pts) {
    history.Push(MakePacket(pts, pts % 3 == 0), 4);
  }
  EXPECT_TRUE(Times(history) == (std::vector<int64_t>{3, 4, 5, 6, 7, 8}));
  EXPECT_EQ(history.bytes(), 60u);

  // On a keyframe: that one starts.
  history.Push(MakePacket(9, true), 6);
  EXPECT_TRUE(Times(history) == (std::vector<int64_t>{6, 7, 8, 9}));
}

TEST(PacketHistory, SkipsPacketsBeforeTheFirstKeyframe) {
  PacketHistory history(1 << 20, false);
  history.Push(MakePacket(0, false), 0);
  history.Push(MakePacket(1, false), 0);
  EXPECT_TRUE(history.packets().empty());
  history.Push(MakePacket(2, true), 0);
  history.Push(MakePacket(3, false), 0);
  EXPECT_TRUE(Times(history) == (std::vector<int64_t>{2, 3}));
}

TEST(PacketHistory, EveryPacketStarts) {
  PacketHistory history(1 << 20, true);
  for (int64_t pts = 0; pts < 5; ++pts) {
    history.Push(MakePacket(pts, false), 2);
  }
  EXPECT_TRUE(Times(history) == (std::vector<int64_t>{2, 3, 4}));
}

TEST(PacketHistory, UnknownTimesAreKept) {
  PacketHistory history(1 << 20, false);
  history.Push(MakePacket(0, true), kNoTimestamp);
  history.Push(MakePacket(kNoTimestamp, true), 5);
  history.Push(MakePacket(2, true), 5);
  // The keyframe without a time cannot be placed: keep from before it.
  EXPECT_EQ(history.packets().size(), 3u);
}

TEST(PacketHistory, DropsOldestKeyframeIntervalPastTheLimit) {
  PacketHistory history(50, false);
  for (int64_t pts = 0; pts < 6; ++pts) {
    history.Push(MakePacket(pts, pts % 3 == 0), 0);
  }
  // 60 bytes: the oldest keyframe interval goes, though it holds the
  // position.
  EXPECT_TRUE(Times(history) == (std::vector<int64_t>{3, 4, 5}));
  EXPECT_EQ(history.bytes(), 30u);

  history.Clear();
  EXPECT_TRUE(history.packets().empty());
  EXPECT_EQ(history.bytes(), 0u);
}

}  // namespace
}  // namespace lychee
