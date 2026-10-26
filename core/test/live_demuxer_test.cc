#include "pipeline/live_demuxer.h"

#include <cstdint>

#include "media/encoded_packet.h"
#include "test.h"

namespace lychee {
namespace {

StreamInfo Audio(AudioCodec codec) {
  StreamInfo info;
  info.type = MediaType::kAudio;
  info.audio.codec = codec;
  return info;
}

int ReadAll(LiveDemuxer* demuxer) {
  int count = 0;
  while (!demuxer->Idle()) {
    EncodedPacket packet;
    if (demuxer->Read(&packet) != DemuxerReadResult::kOk) {
      break;
    }
    demuxer->PacketDelivered();
    ++count;
  }
  return count;
}

void PushTen(LiveDemuxer* demuxer) {
  const uint8_t data[4] = {1, 2, 3, 4};
  for (int i = 0; i < 10; ++i) {
    EXPECT_TRUE(demuxer->Push(LiveDemuxer::kAudioStream, i * 1000, data,
                              sizeof(data)));
  }
}

// The caller changes the track again while the player is still applying
// the first change, then pushes packets for the second.
TEST(LiveDemuxer, ReleaseOfAReplacedHoldKeepsTheNewerOne) {
  LiveDemuxer demuxer(80000, nullptr);
  const int audio = LiveDemuxer::kAudioStream;
  demuxer.SetTrack(audio, Audio(AudioCodec::kALAC));
  const uint64_t first = demuxer.hold(audio);
  demuxer.SetTrack(audio, Audio(AudioCodec::kAAC));
  PushTen(&demuxer);

  // The first change found no decoder: its packets would be dropped and the
  // stream disabled, but the second change's packets are not its own.
  demuxer.Release(audio, false, first);
  EXPECT_TRUE(demuxer.Idle());
  demuxer.Release(audio, true, demuxer.hold(audio));
  EXPECT_EQ(ReadAll(&demuxer), 10);
  PushTen(&demuxer);
  EXPECT_EQ(ReadAll(&demuxer), 10);
}

TEST(LiveDemuxer, ReleaseOfAReplacedHoldDoesNotHandOutTheNewerPackets) {
  LiveDemuxer demuxer(80000, nullptr);
  const int audio = LiveDemuxer::kAudioStream;
  demuxer.SetTrack(audio, Audio(AudioCodec::kAAC));
  const uint64_t first = demuxer.hold(audio);
  demuxer.Flush(audio);
  PushTen(&demuxer);
  // The old renderer must not read them.
  demuxer.Release(audio, true, first);
  EXPECT_TRUE(demuxer.Idle());
  demuxer.Release(audio, true, demuxer.hold(audio));
  EXPECT_EQ(ReadAll(&demuxer), 10);
}

}  // namespace
}  // namespace lychee
