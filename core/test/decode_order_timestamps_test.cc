#include "media/decode_order_timestamps.h"

#include <memory>
#include <vector>

#include "test.h"

// Test registration allocates before main, as all TEST declarations do.
// NOLINTBEGIN(bugprone-throwing-static-initialization)

namespace lychee {
namespace {

// Writes RBSP bits MSB first, then the stop bit and padding.
class BitWriter {
 public:
  void Bits(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) {
      Bit((value >> i) & 1);
    }
  }
  void Ue(uint32_t value) {
    const uint32_t coded = value + 1;
    int bits = 0;
    while ((coded >> bits) > 1) {
      ++bits;
    }
    Bits(0, bits);
    Bits(coded, bits + 1);
  }
  // A NAL unit: |header|, the bits written, the trailing bits.
  std::vector<uint8_t> Nal(uint8_t header) {
    Bit(1);
    while (count_ % 8 != 0) {
      Bit(0);
    }
    std::vector<uint8_t> nal = {header};
    nal.insert(nal.end(), bytes_.begin(), bytes_.end());
    return nal;
  }

 private:
  void Bit(uint32_t bit) {
    if (count_ % 8 == 0) {
      bytes_.push_back(0);
    }
    bytes_.back() |= static_cast<uint8_t>(bit << (7 - count_ % 8));
    ++count_;
  }
  std::vector<uint8_t> bytes_;
  int count_ = 0;
};

// Main profile, POC type 0 with 6-bit lsbs; |reorder| in the VUI, or no
// VUI if negative.
std::vector<uint8_t> Sps(int reorder) {
  BitWriter w;
  w.Bits(77, 8);  // profile_idc
  w.Bits(0, 8);   // constraint flags
  w.Bits(30, 8);  // level_idc
  w.Ue(0);        // seq_parameter_set_id
  w.Ue(0);        // log2_max_frame_num_minus4
  w.Ue(0);        // pic_order_cnt_type
  w.Ue(2);        // log2_max_pic_order_cnt_lsb_minus4
  w.Ue(2);        // max_num_ref_frames
  w.Bits(0, 1);   // gaps_in_frame_num_value_allowed_flag
  w.Ue(19);       // pic_width_in_mbs_minus1
  w.Ue(14);       // pic_height_in_map_units_minus1
  w.Bits(1, 1);   // frame_mbs_only_flag
  w.Bits(1, 1);   // direct_8x8_inference_flag
  w.Bits(0, 1);   // frame_cropping_flag
  w.Bits(reorder >= 0, 1);
  if (reorder >= 0) {
    w.Bits(0, 8);  // aspect ratio ... pic_struct: none present
    w.Bits(1, 1);  // bitstream_restriction_flag
    w.Bits(1, 1);  // motion_vectors_over_pic_boundaries_flag
    for (int i = 0; i < 4; ++i) {
      w.Ue(0);
    }
    w.Ue(static_cast<uint32_t>(reorder));  // max_num_reorder_frames
    w.Ue(4);                               // max_dec_frame_buffering
  }
  return w.Nal(0x67);
}

std::vector<uint8_t> Pps() {
  BitWriter w;
  w.Ue(0);       // pic_parameter_set_id
  w.Ue(0);       // seq_parameter_set_id
  w.Bits(0, 1);  // entropy_coding_mode_flag
  w.Bits(0, 1);  // bottom_field_pic_order_in_frame_present_flag
  return w.Nal(0x68);
}

VideoConfig H264(int reorder) {
  std::vector<uint8_t> sps = Sps(reorder);
  std::vector<uint8_t> pps = Pps();
  VideoConfig config;
  config.codec = VideoCodec::kH264;
  config.extradata = {1,    77,   0, 30,
                      0xff, 0xe1, 0, static_cast<uint8_t>(sps.size())};
  config.extradata.insert(config.extradata.end(), sps.begin(), sps.end());
  config.extradata.push_back(1);
  config.extradata.push_back(0);
  config.extradata.push_back(static_cast<uint8_t>(pps.size()));
  config.extradata.insert(config.extradata.end(), pps.begin(), pps.end());
  return config;
}

// A one-slice picture with POC |order|, at |time| ms in decode order.
std::shared_ptr<EncodedPacket> Picture(int order,
                                       int64_t time,
                                       bool idr = false,
                                       bool reference = true) {
  BitWriter w;
  w.Ue(0);            // first_mb_in_slice
  w.Ue(idr ? 7 : 5);  // slice_type: I or P (B is the same here)
  w.Ue(0);            // pic_parameter_set_id
  w.Bits(0, 4);       // frame_num
  if (idr) {
    w.Ue(0);  // idr_pic_id
  }
  w.Bits(static_cast<uint32_t>(order) & 63, 6);  // pic_order_cnt_lsb
  const uint8_t header =
      static_cast<uint8_t>((reference ? 0x60 : 0) | (idr ? 5 : 1));
  std::vector<uint8_t> nal = w.Nal(header);
  std::vector<uint8_t> sample = {0, 0, 0, static_cast<uint8_t>(nal.size())};
  sample.insert(sample.end(), nal.begin(), nal.end());
  auto packet = std::make_shared<EncodedPacket>();
  packet->pts = packet->dts = time * 1000;
  packet->duration = 1000;
  packet->keyframe = idr;
  packet->buffer = PacketBuffer::Copy(sample.data(), sample.size());
  return packet;
}

std::vector<std::shared_ptr<EncodedPacket>> PopAll(
    DecodeOrderTimestamps* timestamps) {
  std::vector<std::shared_ptr<EncodedPacket>> packets;
  while (std::shared_ptr<EncodedPacket> packet = timestamps->Pop()) {
    packets.push_back(std::move(packet));
  }
  return packets;
}

// I0 P3 B1 B2 P6 B4 B5 in decode order (POC twice the display index).
const int kOrders[] = {0, 6, 2, 4, 12, 8, 10};
const int64_t kDisplayTimes[] = {0, 3000, 1000, 2000, 6000, 4000, 5000};

TEST(DecodeOrderTimestamps, HandsOutTimesInPictureOrder) {
  for (int reorder : {-1, 2}) {
    DecodeOrderTimestamps timestamps;
    timestamps.Initialize(H264(reorder));
    std::vector<std::shared_ptr<EncodedPacket>> out;
    for (int i = 0; i < 7; ++i) {
      timestamps.Push(Picture(kOrders[i], i, i == 0));
      for (auto& packet : PopAll(&timestamps)) {
        out.push_back(std::move(packet));
      }
    }
    // A VUI reorder limit lets packets go before the end.
    EXPECT_EQ(out.size() > 0, reorder >= 0);
    timestamps.Drain();
    for (auto& packet : PopAll(&timestamps)) {
      out.push_back(std::move(packet));
    }
    EXPECT_EQ(out.size(), 7u);
    int64_t last_dts = INT64_MIN;
    for (size_t i = 0; i < out.size(); ++i) {
      EXPECT_EQ(out[i]->pts, kDisplayTimes[i]);
      // A dts no later packet's pts comes before.
      EXPECT_TRUE(out[i]->dts <= out[i]->pts && out[i]->dts >= last_dts);
      for (size_t j = i; j < out.size(); ++j) {
        EXPECT_TRUE(out[j]->pts >= out[i]->dts);
      }
      last_dts = out[i]->dts;
    }
  }
}

TEST(DecodeOrderTimestamps, KeepsTheReadThreadsPackets) {
  DecodeOrderTimestamps timestamps;
  timestamps.Initialize(H264(-1));
  std::shared_ptr<EncodedPacket> p3 = Picture(6, 1);
  timestamps.Push(Picture(0, 0, true));
  timestamps.Push(p3);
  timestamps.Push(Picture(2, 2));
  timestamps.Drain();
  std::vector<std::shared_ptr<EncodedPacket>> out = PopAll(&timestamps);
  EXPECT_EQ(out.size(), 3u);
  EXPECT_EQ(out[1]->pts, 2000);
  EXPECT_EQ(p3->pts, 1000);
}

TEST(DecodeOrderTimestamps, StartsOverAtIdrPictures) {
  DecodeOrderTimestamps timestamps;
  timestamps.Initialize(H264(-1));
  // POC drops to 0 at the second IDR; its pictures still come after.
  const int orders[] = {0, 4, 2, 0, 4, 2};
  for (int i = 0; i < 6; ++i) {
    timestamps.Push(Picture(orders[i], i, i % 3 == 0));
  }
  timestamps.Drain();
  std::vector<std::shared_ptr<EncodedPacket>> out = PopAll(&timestamps);
  const int64_t expected[] = {0, 2000, 1000, 3000, 5000, 4000};
  EXPECT_EQ(out.size(), 6u);
  for (size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i]->pts, expected[i]);
  }
}

TEST(DecodeOrderTimestamps, LeavesDisplayTimesAlone) {
  DecodeOrderTimestamps timestamps;
  timestamps.Initialize(H264(-1));
  // A container with composition offsets: pts differs from dts.
  for (int i = 0; i < 4; ++i) {
    std::shared_ptr<EncodedPacket> packet = Picture(kOrders[i], i, i == 0);
    packet->pts = kDisplayTimes[i] + 1000;
    timestamps.Push(packet);
    std::vector<std::shared_ptr<EncodedPacket>> out = PopAll(&timestamps);
    EXPECT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0] == packet);
    EXPECT_EQ(out[0]->pts, kDisplayTimes[i] + 1000);
  }
}

TEST(DecodeOrderTimestamps, PassesOtherCodecsThrough) {
  DecodeOrderTimestamps timestamps;
  VideoConfig config = H264(-1);
  config.codec = VideoCodec::kHEVC;
  timestamps.Initialize(config);
  std::shared_ptr<EncodedPacket> packet = Picture(6, 1);
  timestamps.Push(packet);
  EXPECT_TRUE(timestamps.Pop() == packet);
}

}  // namespace
}  // namespace lychee

// NOLINTEND(bugprone-throwing-static-initialization)
