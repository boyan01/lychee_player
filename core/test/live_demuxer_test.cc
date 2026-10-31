#include "pipeline/live_demuxer.h"

#include <algorithm>
#include <cstdint>
#include <vector>

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

StreamInfo H264() {
  StreamInfo info;
  info.type = MediaType::kVideo;
  info.video.codec = VideoCodec::kH264;
  return info;
}

// One H.264 NAL unit of |type| in Annex B, |size| bytes in all.
std::vector<uint8_t> Nal(int type, size_t size = 8) {
  std::vector<uint8_t> data(std::max<size_t>(size, 5), 0x55);
  data[0] = data[1] = data[2] = 0;
  data[3] = 1;
  data[4] = static_cast<uint8_t>(type);
  return data;
}

bool PushVideo(LiveDemuxer* demuxer, int type, size_t size = 8) {
  const std::vector<uint8_t> data = Nal(type, size);
  return demuxer->Push(LiveDemuxer::kVideoStream, 0, data.data(), data.size());
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

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(LiveDemuxer, CountsVideoDroppedBeforeAKeyframe) {
  LiveDemuxer demuxer(80000, nullptr);
  const int video = LiveDemuxer::kVideoStream;
  demuxer.SetTrack(video, H264());
  demuxer.Release(video, true, demuxer.hold(video));
  PushVideo(&demuxer, 1);
  PushVideo(&demuxer, 1);
  EXPECT_EQ(demuxer.video_packets_dropped(), 2);
  PushVideo(&demuxer, 5);
  PushVideo(&demuxer, 1);
  EXPECT_EQ(demuxer.video_packets_dropped(), 2);
  EXPECT_EQ(ReadAll(&demuxer), 2);

  // What a flush discards was asked for; what follows it before a keyframe
  // was not.
  PushVideo(&demuxer, 1);
  demuxer.Flush(video);
  EXPECT_EQ(demuxer.video_packets_dropped(), 2);
  PushVideo(&demuxer, 1);
  EXPECT_EQ(demuxer.video_packets_dropped(), 3);
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(LiveDemuxer, CountsVideoDroppedByAnOverflowAndANewDecoder) {
  LiveDemuxer demuxer(80000, nullptr);
  const int video = LiveDemuxer::kVideoStream;
  demuxer.SetTrack(video, H264());
  demuxer.Release(video, true, demuxer.hold(video));
  // 1 MB pictures that nothing reads: the 17th overflows 16 MB and drops
  // the 16 queued, then is dropped itself as no keyframe.
  PushVideo(&demuxer, 5, 1 << 20);
  for (int i = 0; i < 16; ++i) {
    PushVideo(&demuxer, 1, 1 << 20);
  }
  EXPECT_EQ(demuxer.video_packets_dropped(), 17);
  EXPECT_TRUE(demuxer.Idle());

  PushVideo(&demuxer, 5);
  PushVideo(&demuxer, 1);
  EXPECT_EQ(ReadAll(&demuxer), 2);
  PushVideo(&demuxer, 1);
  PushVideo(&demuxer, 1);
  // A new decoder starts at the next keyframe.
  demuxer.RestartVideo();
  EXPECT_EQ(demuxer.video_packets_dropped(), 19);
  EXPECT_TRUE(demuxer.Idle());
}

}  // namespace
}  // namespace lychee
