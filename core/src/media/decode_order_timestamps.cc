#include "media/decode_order_timestamps.h"

#include <algorithm>
#include <functional>
#include <utility>

#include "base/log.h"
#include "media/bit_reader.h"

namespace lychee {

namespace {

// An SPS is a few dozen bytes; more is never needed for the fields read.
constexpr size_t kMaxSpsBytes = 4096;
// Enough for a slice header up to pic_order_cnt_lsb and delta_pic_order_cnt
// (at most about 30 bytes).
constexpr size_t kMaxSliceHeaderBytes = 64;
// The largest DPB (A.3.1): with no VUI bitstream restriction, a picture
// may follow this many decoded after it.
constexpr size_t kMaxReorder = 16;

constexpr int kNalSlice = 1;
constexpr int kNalIdr = 5;
constexpr int kNalSps = 7;
constexpr int kNalPps = 8;

void SkipHrd(BitReader* reader) {
  const uint32_t count = reader->Ue() + 1;
  reader->Skip(8);  // bit_rate_scale, cpb_size_scale
  for (uint32_t i = 0; i < count && i < 32 && reader->ok(); ++i) {
    reader->Ue();    // bit_rate_value_minus1
    reader->Ue();    // cpb_size_value_minus1
    reader->Flag();  // cbr_flag
  }
  // initial_cpb_removal_delay_length_minus1, cpb_removal_delay_length_minus1,
  // dpb_output_delay_length_minus1, time_offset_length.
  reader->Skip(20);
}

}  // namespace

void DecodeOrderTimestamps::Initialize(const VideoConfig& config) {
  *this = DecodeOrderTimestamps();
  enabled_ = config.codec == VideoCodec::kH264 && !config.low_delay &&
             nal_.Initialize(config);
  active_ = enabled_;
  for (const std::vector<uint8_t>& set : nal_.parameter_sets()) {
    if (set.empty()) {
      continue;
    }
    if ((set[0] & 0x1f) == kNalSps) {
      ParseSps(set.data(), set.size());
    } else if ((set[0] & 0x1f) == kNalPps) {
      ParsePps(set.data(), set.size());
    }
  }
}

void DecodeOrderTimestamps::Push(std::shared_ptr<EncodedPacket> packet) {
  if (active_ && (packet->pts == kNoTimestamp || packet->pts != packet->dts)) {
    // The container has display times.
    Stop();
  }
  int64_t order = 0;
  bool idr = false;
  if (active_ && !PictureOrder(*packet, &order, &idr)) {
    LOGW("cannot tell the picture order: decode times kept as display times");
    Stop();
  }
  if (!active_) {
    ready_.push_back(std::move(packet));
    return;
  }
  if (idr) {
    // Pictures before an IDR picture are output before it (C.4.4).
    while (untimed_ > 0) {
      TimeNext();
    }
  }
  slots_.push_back({packet->pts, packet->duration});
  std::push_heap(slots_.begin(), slots_.end(), std::greater<Slot>());
  // A copy: the read thread keeps packets for resyncs (PacketHistory).
  held_.push_back({std::make_shared<EncodedPacket>(*packet), order, false});
  ++untimed_;
  // What a decoder's DPB outputs once it holds more than |reorder_|
  // pictures waiting to be output (C.4.5.3, bumping).
  while (untimed_ > reorder_) {
    TimeNext();
  }
  Release();
}

void DecodeOrderTimestamps::Drain() {
  while (untimed_ > 0) {
    TimeNext();
  }
  Release();
}

std::shared_ptr<EncodedPacket> DecodeOrderTimestamps::Pop() {
  if (ready_.empty()) {
    return nullptr;
  }
  std::shared_ptr<EncodedPacket> packet = std::move(ready_.front());
  ready_.pop_front();
  return packet;
}

void DecodeOrderTimestamps::Reset() {
  held_.clear();
  slots_.clear();
  ready_.clear();
  untimed_ = 0;
  first_ = true;
}

void DecodeOrderTimestamps::TimeNext() {
  Held* next = nullptr;
  for (Held& held : held_) {
    if (!held.timed && (!next || held.order < next->order)) {
      next = &held;
    }
  }
  std::pop_heap(slots_.begin(), slots_.end(), std::greater<Slot>());
  next->packet->pts = slots_.back().time;
  next->packet->duration = slots_.back().duration;
  slots_.pop_back();
  next->timed = true;
  --untimed_;
}

void DecodeOrderTimestamps::Release() {
  while (!held_.empty() && held_.front().timed) {
    // Every packet after this one gets a time at or after the earliest
    // held: a dts no later packet's pts comes before, as decoders that
    // reorder by pts rely on (VTVideoDecoder).
    int64_t dts = held_.front().packet->pts;
    for (const Held& held : held_) {
      if (held.timed) {
        dts = std::min(dts, held.packet->pts);
      }
    }
    held_.front().packet->dts = dts;
    ready_.push_back(std::move(held_.front().packet));
    held_.pop_front();
  }
}

void DecodeOrderTimestamps::Stop() {
  Drain();
  active_ = false;
}

void DecodeOrderTimestamps::ParseSps(const uint8_t* nal, size_t size) {
  if (size < 2) {
    return;
  }
  std::vector<uint8_t> rbsp = Rbsp(nal + 1, size - 1, kMaxSpsBytes);
  BitReader reader(rbsp.data(), rbsp.size());
  Sps sps;
  const uint32_t profile = reader.Bits(8);
  reader.Skip(16);  // constraint flags, level_idc
  const uint32_t id = reader.Ue();
  switch (profile) {
    case 100:
    case 110:
    case 122:
    case 244:
    case 44:
    case 83:
    case 86:
    case 118:
    case 128:
    case 138:
    case 139:
    case 134:
    case 135: {
      const uint32_t chroma_format = reader.Ue();
      if (chroma_format == 3) {
        sps.separate_planes = reader.Flag();
      }
      reader.Ue();          // bit_depth_luma_minus8
      reader.Ue();          // bit_depth_chroma_minus8
      reader.Flag();        // qpprime_y_zero_transform_bypass_flag
      if (reader.Flag()) {  // seq_scaling_matrix_present_flag
        const int lists = chroma_format == 3 ? 12 : 8;
        for (int i = 0; i < lists; ++i) {
          if (reader.Flag()) {
            SkipH264ScalingList(&reader, i < 6 ? 16 : 64);
          }
        }
      }
      break;
    }
    default:
      break;
  }
  sps.log2_max_frame_num = static_cast<int>(reader.Ue()) + 4;
  sps.poc_type = static_cast<int>(reader.Ue());
  if (sps.poc_type == 0) {
    sps.log2_max_poc_lsb = static_cast<int>(reader.Ue()) + 4;
  } else if (sps.poc_type == 1) {
    reader.Flag();  // delta_pic_order_always_zero_flag
    reader.Se();    // offset_for_non_ref_pic
    reader.Se();    // offset_for_top_to_bottom_field
    const uint32_t cycle = reader.Ue();
    for (uint32_t i = 0; i < cycle && i < 256 && reader.ok(); ++i) {
      reader.Se();
    }
  }
  reader.Ue();    // max_num_ref_frames
  reader.Flag();  // gaps_in_frame_num_value_allowed_flag
  reader.Ue();    // pic_width_in_mbs_minus1
  reader.Ue();    // pic_height_in_map_units_minus1
  sps.frame_mbs_only = reader.Flag();
  if (!sps.frame_mbs_only) {
    reader.Flag();  // mb_adaptive_frame_field_flag
  }
  reader.Flag();        // direct_8x8_inference_flag
  if (reader.Flag()) {  // frame_cropping_flag
    for (int i = 0; i < 4; ++i) {
      reader.Ue();
    }
  }
  // Baseline has no B slices; others say how far pictures are reordered
  // only in the VUI (E.1.1), and may reorder as far as the DPB holds.
  sps.reorder = profile == 66 ? 0 : kMaxReorder;
  if (reader.Flag()) {    // vui_parameters_present_flag
    if (reader.Flag()) {  // aspect_ratio_info_present_flag
      if (reader.Bits(8) == 255) {
        reader.Skip(32);  // sar_width, sar_height
      }
    }
    if (reader.Flag()) {  // overscan_info_present_flag
      reader.Flag();
    }
    if (reader.Flag()) {  // video_signal_type_present_flag
      reader.Skip(4);     // video_format, video_full_range_flag
      if (reader.Flag()) {
        reader.Skip(24);  // colour_primaries, transfer, matrix
      }
    }
    if (reader.Flag()) {  // chroma_loc_info_present_flag
      reader.Ue();
      reader.Ue();
    }
    if (reader.Flag()) {  // timing_info_present_flag
      reader.Skip(65);
    }
    const bool nal_hrd = reader.Flag();
    if (nal_hrd) {
      SkipHrd(&reader);
    }
    const bool vcl_hrd = reader.Flag();
    if (vcl_hrd) {
      SkipHrd(&reader);
    }
    if (nal_hrd || vcl_hrd) {
      reader.Flag();  // low_delay_hrd_flag
    }
    reader.Flag();        // pic_struct_present_flag
    if (reader.Flag()) {  // bitstream_restriction_flag
      reader.Flag();      // motion_vectors_over_pic_boundaries_flag
      for (int i = 0; i < 4; ++i) {
        reader.Ue();  // max_bytes_per_pic_denom ... log2_max_mv_length_vertical
      }
      const uint32_t reorder = reader.Ue();  // max_num_reorder_frames
      if (reader.ok() && reorder <= kMaxReorder) {
        sps.reorder = reorder;
      }
    }
  }
  if (id < 32 && sps.log2_max_frame_num <= 16 && sps.log2_max_poc_lsb <= 16) {
    sps_[id] = sps;
  }
}

void DecodeOrderTimestamps::ParsePps(const uint8_t* nal, size_t size) {
  if (size < 2) {
    return;
  }
  std::vector<uint8_t> rbsp = Rbsp(nal + 1, size - 1, kMaxSliceHeaderBytes);
  BitReader reader(rbsp.data(), rbsp.size());
  const uint32_t id = reader.Ue();
  Pps pps;
  pps.sps = static_cast<int>(reader.Ue());
  reader.Flag();  // entropy_coding_mode_flag
  pps.bottom_field_poc = reader.Flag();
  if (reader.ok() && id < 256) {
    pps_[id] = pps;
  }
}

bool DecodeOrderTimestamps::PictureOrder(const EncodedPacket& packet,
                                         int64_t* order,
                                         bool* idr) {
  if (!packet.buffer) {
    return false;
  }
  const uint8_t* data = packet.buffer->data();
  const size_t size = packet.size();
  const size_t length_size = static_cast<size_t>(nal_.length_size());
  // NAL units, length-prefixed (MP4) or after start codes (Annex B).
  size_t position = 0;
  while (position < size) {
    size_t begin;
    size_t end;
    if (length_size > 0) {
      if (size - position < length_size) {
        return false;
      }
      size_t length = 0;
      for (size_t i = 0; i < length_size; ++i) {
        length = (length << 8) | data[position + i];
      }
      begin = position + length_size;
      if (length > size - begin) {
        return false;
      }
      end = begin + length;
    } else {
      // From the start code at or after |position| to the next one.
      begin = size;
      for (size_t i = position; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
          begin = i + 3;
          break;
        }
      }
      end = size;
      for (size_t i = begin; i + 3 <= size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] <= 1) {
          end = i;
          break;
        }
      }
    }
    position = end;
    if (begin >= end) {
      continue;
    }
    const int type = data[begin] & 0x1f;
    if (type == kNalSps) {
      ParseSps(data + begin, end - begin);
    } else if (type == kNalPps) {
      ParsePps(data + begin, end - begin);
    } else if (type == kNalSlice || type == kNalIdr) {
      return SliceOrder(data + begin, end - begin, order, idr);
    }
  }
  return false;
}

// 7.3.3 up to delta_pic_order_cnt_bottom, and 8.2.1.1 (POC type 0).
bool DecodeOrderTimestamps::SliceOrder(const uint8_t* nal,
                                       size_t size,
                                       int64_t* order,
                                       bool* idr) {
  if (size < 2) {
    return false;
  }
  const bool reference = ((nal[0] >> 5) & 3) != 0;
  *idr = (nal[0] & 0x1f) == kNalIdr;
  std::vector<uint8_t> rbsp = Rbsp(nal + 1, size - 1, kMaxSliceHeaderBytes);
  BitReader reader(rbsp.data(), rbsp.size());
  reader.Ue();  // first_mb_in_slice
  reader.Ue();  // slice_type
  auto pps = pps_.find(reader.Ue());
  if (pps == pps_.end()) {
    return false;
  }
  auto found = sps_.find(static_cast<uint32_t>(pps->second.sps));
  if (found == sps_.end()) {
    return false;
  }
  const Sps& sps = found->second;
  reorder_ = sps.reorder;
  if (sps.poc_type == 2) {
    // Output order is decode order (8.2.1.3).
    *order = decoded_++;
    return true;
  }
  if (sps.poc_type != 0) {
    return false;
  }
  if (sps.separate_planes) {
    reader.Skip(2);  // colour_plane_id
  }
  reader.Skip(sps.log2_max_frame_num);  // frame_num
  bool field = false;
  if (!sps.frame_mbs_only) {
    field = reader.Flag();
    if (field) {
      reader.Flag();  // bottom_field_flag
    }
  }
  if (*idr) {
    reader.Ue();  // idr_pic_id
  }
  const int64_t lsb = reader.Bits(sps.log2_max_poc_lsb);
  int64_t delta_bottom = 0;
  if (pps->second.bottom_field_poc && !field) {
    delta_bottom = reader.Se();
  }
  if (!reader.ok()) {
    return false;
  }
  if (*idr) {
    previous_msb_ = 0;
    previous_lsb_ = 0;
  } else if (first_) {
    // After a seek to a picture that is not IDR: orders from here on are
    // relative to it.
    previous_msb_ = 0;
    previous_lsb_ = lsb;
  }
  first_ = false;
  const int64_t max_lsb = int64_t{1} << sps.log2_max_poc_lsb;
  int64_t msb = previous_msb_;
  if (lsb < previous_lsb_ && previous_lsb_ - lsb >= max_lsb / 2) {
    msb += max_lsb;
  } else if (lsb > previous_lsb_ && lsb - previous_lsb_ > max_lsb / 2) {
    msb -= max_lsb;
  }
  // A field's order, or a frame's: the earlier of its two fields.
  const int64_t top = msb + lsb;
  *order = field ? top : std::min(top, top + delta_bottom);
  if (reference) {
    previous_msb_ = msb;
    previous_lsb_ = lsb;
  }
  return true;
}

}  // namespace lychee
