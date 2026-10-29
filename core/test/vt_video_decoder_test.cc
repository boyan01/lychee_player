#include "backends/apple/vt_video_decoder.h"

#include "test.h"

namespace lychee {
namespace {

// One black 16x16 IDR, encoded with libx264, baseline profile, keyint=1.
// Keep the SPS/PPS and slice separate, as a file demuxer supplies them.
constexpr uint8_t kParameterSets[] = {
    0,    0,    0,    1,    0x67, 0x42, 0xc0, 0x1e, 0xdd, 0xec, 0x04,
    0x40, 0x00, 0x00, 0x03, 0x00, 0x40, 0x00, 0x00, 0x0f, 0x03, 0xc5,
    0x8b, 0xe0, 0,    0,    0,    1,    0x68, 0xce, 0x0f, 0x2c, 0x80};
constexpr uint8_t kPicture[] = {0,    0,    0,    1,    0x65, 0x88, 0x84, 0x04,
                                0xbc, 0x98, 0xa0, 0x00, 0x38, 0xa3, 0x80};

// Registration uses the test harness's process-lifetime vector.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
TEST(VTVideoDecoder, SoftwareOutputsFileFramesWithoutWaitingForMorePackets) {
  VideoConfig config;
  config.codec = VideoCodec::kH264;
  config.codec_name = "h264";
  config.width = 16;
  config.height = 16;
  config.extradata.assign(std::begin(kParameterSets), std::end(kParameterSets));
  // File playback, rather than the already-synchronous low-delay mode.
  EXPECT_TRUE(!config.low_delay);
  VTVideoDecoder decoder(false);
  std::string error;
  const bool initialized = decoder.Initialize(config, &error);
  EXPECT_TRUE(initialized);
  EXPECT_EQ(error, std::string());
  if (!initialized) {
    return;
  }
  EXPECT_TRUE(!decoder.is_hardware());

  EncodedPacket packet;
  packet.buffer = PacketBuffer::Copy(kPicture, sizeof(kPicture));
  packet.keyframe = true;
  packet.pts = packet.dts = 0;
  packet.duration = kMicrosPerSecond / 30;
  int frames = 0;
  auto output = [&](const std::shared_ptr<VideoFrame>& frame) {
    ++frames;
    EXPECT_EQ(frame->pts, packet.pts);
    EXPECT_EQ(frame->width, 16);
    EXPECT_EQ(frame->height, 16);
  };
  EXPECT_TRUE(decoder.Decode(packet, output));
  EXPECT_EQ(frames, 1);

  // Seeking must not reintroduce a wait for a full software batch.
  decoder.Flush();
  packet.pts = packet.dts = kMicrosPerSecond;
  EXPECT_TRUE(decoder.Decode(packet, output));
  EXPECT_EQ(frames, 2);
  EXPECT_TRUE(decoder.Decode(*EncodedPacket::CreateEndOfStream(0), output));
  EXPECT_EQ(frames, 2);
}

}  // namespace
}  // namespace lychee
