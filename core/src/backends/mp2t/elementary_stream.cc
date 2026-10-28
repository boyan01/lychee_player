#include "backends/mp2t/elementary_stream.h"

#include <vector>

namespace lychee {

namespace {

// An SPS is a few dozen bytes; more is never needed for the fields read.
constexpr size_t kMaxSpsBytes = 4096;

// Bits of an RBSP (emulation prevention bytes removed), MSB first. Reads
// past the end yield zeros and clear ok().
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  bool ok() const { return ok_; }

  uint32_t Bits(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      if (position_ >= size_ * 8) {
        ok_ = false;
        return 0;
      }
      value =
          (value << 1) | ((data_[position_ / 8] >> (7 - position_ % 8)) & 1);
      ++position_;
    }
    return value;
  }
  bool Flag() { return Bits(1) != 0; }
  void Skip(int count) {
    for (; count > 32; count -= 32) {
      Bits(32);
    }
    Bits(count);
  }
  // Exp-Golomb, ue(v). Values above 2^31 are malformed here.
  uint32_t Ue() {
    int zeros = 0;
    while (!Flag()) {
      if (!ok_ || ++zeros > 31) {
        ok_ = false;
        return 0;
      }
    }
    return (1u << zeros) - 1 + Bits(zeros);
  }
  int32_t Se() {
    uint32_t value = Ue();
    return value & 1 ? static_cast<int32_t>((value + 1) / 2)
                     : -static_cast<int32_t>(value / 2);
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t position_ = 0;
  bool ok_ = true;
};

// The NAL unit's payload without emulation prevention bytes (00 00 03).
std::vector<uint8_t> Rbsp(const uint8_t* nal, size_t size) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size < kMaxSpsBytes ? size : kMaxSpsBytes);
  int zeros = 0;
  for (size_t i = 0; i < size && rbsp.size() < kMaxSpsBytes; ++i) {
    if (zeros >= 2 && nal[i] == 3) {
      zeros = 0;
      continue;
    }
    zeros = nal[i] == 0 ? zeros + 1 : 0;
    rbsp.push_back(nal[i]);
  }
  return rbsp;
}

// Table E-1 of ITU-T H.264 (and H.265), aspect_ratio_idc 1 to 16.
constexpr int kSampleAspectRatios[16][2] = {
    {1, 1},    {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11},
    {20, 11},  {32, 11}, {80, 33}, {18, 11}, {15, 11}, {64, 33},
    {160, 99}, {4, 3},   {3, 2},   {2, 1}};

void SkipScalingList(BitReader* reader, int size) {
  int last = 8;
  int next = 8;
  for (int i = 0; i < size && reader->ok(); ++i) {
    if (next != 0) {
      next = (last + reader->Se() + 256) % 256;
    }
    last = next == 0 ? last : next;
  }
}

// ITU-T H.264 7.3.2.1.1, up to the VUI's colour description.
bool ParseH264Sps(const uint8_t* nal, size_t size, AccessUnitInfo* info) {
  std::vector<uint8_t> rbsp = Rbsp(nal + 1, size - 1);
  BitReader reader(rbsp.data(), rbsp.size());
  const uint32_t profile = reader.Bits(8);
  reader.Skip(16);  // constraint flags, level_idc
  reader.Ue();      // seq_parameter_set_id
  uint32_t chroma_format = 1;
  bool separate_planes = false;
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
    case 135:
      chroma_format = reader.Ue();
      if (chroma_format > 3) {
        return false;
      }
      if (chroma_format == 3) {
        separate_planes = reader.Flag();
      }
      reader.Ue();          // bit_depth_luma_minus8
      reader.Ue();          // bit_depth_chroma_minus8
      reader.Flag();        // qpprime_y_zero_transform_bypass_flag
      if (reader.Flag()) {  // seq_scaling_matrix_present_flag
        const int lists = chroma_format == 3 ? 12 : 8;
        for (int i = 0; i < lists; ++i) {
          if (reader.Flag()) {
            SkipScalingList(&reader, i < 6 ? 16 : 64);
          }
        }
      }
      break;
    default:
      break;
  }
  reader.Ue();  // log2_max_frame_num_minus4
  const uint32_t poc_type = reader.Ue();
  if (poc_type == 0) {
    reader.Ue();  // log2_max_pic_order_cnt_lsb_minus4
  } else if (poc_type == 1) {
    reader.Flag();  // delta_pic_order_always_zero_flag
    reader.Se();    // offset_for_non_ref_pic
    reader.Se();    // offset_for_top_to_bottom_field
    const uint32_t cycle = reader.Ue();
    if (cycle > 255) {
      return false;
    }
    for (uint32_t i = 0; i < cycle; ++i) {
      reader.Se();
    }
  }
  reader.Ue();    // max_num_ref_frames
  reader.Flag();  // gaps_in_frame_num_value_allowed_flag
  const uint32_t width_mbs = reader.Ue() + 1;
  const uint32_t height_units = reader.Ue() + 1;
  const bool frame_mbs_only = reader.Flag();
  if (!frame_mbs_only) {
    reader.Flag();  // mb_adaptive_frame_field_flag
  }
  reader.Flag();                    // direct_8x8_inference_flag
  uint32_t crop[4] = {0, 0, 0, 0};  // left, right, top, bottom
  if (reader.Flag()) {
    for (uint32_t& value : crop) {
      value = reader.Ue();
    }
  }
  if (!reader.ok() || width_mbs > 1024 || height_units > 1024) {
    return false;
  }
  // 7.4.2.1.1: crop units by ChromaArrayType.
  const uint32_t chroma_array = separate_planes ? 0 : chroma_format;
  const uint32_t sub_width = chroma_array == 1 || chroma_array == 2 ? 2 : 1;
  const uint32_t sub_height = chroma_array == 1 ? 2 : 1;
  const uint32_t field_factor = frame_mbs_only ? 1 : 2;
  const uint32_t crop_x = chroma_array == 0 ? 1 : sub_width;
  const uint32_t crop_y = (chroma_array == 0 ? 1 : sub_height) * field_factor;
  const int64_t width = static_cast<int64_t>(width_mbs) * 16 -
                        static_cast<int64_t>(crop_x) * (crop[0] + crop[1]);
  const int64_t height =
      static_cast<int64_t>(height_units) * 16 * field_factor -
      static_cast<int64_t>(crop_y) * (crop[2] + crop[3]);
  if (width <= 0 || height <= 0) {
    return false;
  }
  info->width = static_cast<int>(width);
  info->height = static_cast<int>(height);
  info->sar_num = info->sar_den = 1;
  info->color = ColorInfo();
  if (reader.Flag()) {    // vui_parameters_present_flag
    if (reader.Flag()) {  // aspect_ratio_info_present_flag
      const uint32_t idc = reader.Bits(8);
      if (idc == 255) {
        info->sar_num = static_cast<int>(reader.Bits(16));
        info->sar_den = static_cast<int>(reader.Bits(16));
      } else if (idc >= 1 && idc <= 16) {
        info->sar_num = kSampleAspectRatios[idc - 1][0];
        info->sar_den = kSampleAspectRatios[idc - 1][1];
      }
      if (info->sar_num <= 0 || info->sar_den <= 0) {
        info->sar_num = info->sar_den = 1;
      }
    }
    if (reader.Flag()) {  // overscan_info_present_flag
      reader.Flag();
    }
    if (reader.Flag()) {  // video_signal_type_present_flag
      reader.Skip(3);     // video_format
      const bool full_range = reader.Flag();
      if (reader.Flag()) {  // colour_description_present_flag
        ColorInfo color;
        color.primaries = static_cast<uint8_t>(reader.Bits(8));
        color.transfer = static_cast<uint8_t>(reader.Bits(8));
        color.matrix = static_cast<uint8_t>(reader.Bits(8));
        color.full_range = full_range;
        if (reader.ok()) {
          info->color = color;
        }
      } else {
        info->color.full_range = full_range;
      }
    }
  }
  return true;
}

// ITU-T H.265 7.3.2.2.1, up to the conformance window. The VUI (sample
// aspect ratio) lies past fields that need far more parsing; HEVC in
// MPEG-TS has square pixels in practice.
bool ParseHevcSps(const uint8_t* nal, size_t size, AccessUnitInfo* info) {
  if (size < 3) {
    return false;
  }
  std::vector<uint8_t> rbsp = Rbsp(nal + 2, size - 2);
  BitReader reader(rbsp.data(), rbsp.size());
  reader.Skip(4);  // sps_video_parameter_set_id
  const uint32_t sub_layers = reader.Bits(3);
  reader.Skip(1);  // sps_temporal_id_nesting_flag
  // profile_tier_level(1, sps_max_sub_layers_minus1)
  reader.Skip(96);
  bool profile_present[8] = {};
  bool level_present[8] = {};
  for (uint32_t i = 0; i < sub_layers; ++i) {
    profile_present[i] = reader.Flag();
    level_present[i] = reader.Flag();
  }
  if (sub_layers > 0) {
    reader.Skip(2 * (8 - static_cast<int>(sub_layers)));
  }
  for (uint32_t i = 0; i < sub_layers; ++i) {
    if (profile_present[i]) {
      reader.Skip(88);
    }
    if (level_present[i]) {
      reader.Skip(8);
    }
  }
  reader.Ue();  // sps_seq_parameter_set_id
  const uint32_t chroma_format = reader.Ue();
  bool separate_planes = false;
  if (chroma_format == 3) {
    separate_planes = reader.Flag();
  }
  const uint32_t width = reader.Ue();
  const uint32_t height = reader.Ue();
  uint32_t window[4] = {0, 0, 0, 0};  // left, right, top, bottom
  if (reader.Flag()) {
    for (uint32_t& value : window) {
      value = reader.Ue();
    }
  }
  if (!reader.ok() || chroma_format > 3 || width == 0 || height == 0 ||
      width > 16384 || height > 16384) {
    return false;
  }
  const uint32_t chroma_array = separate_planes ? 0 : chroma_format;
  const int64_t sub_width = chroma_array == 1 || chroma_array == 2 ? 2 : 1;
  const int64_t sub_height = chroma_array == 1 ? 2 : 1;
  const int64_t shown_width =
      static_cast<int64_t>(width) - sub_width * (window[0] + window[1]);
  const int64_t shown_height =
      static_cast<int64_t>(height) - sub_height * (window[2] + window[3]);
  if (shown_width <= 0 || shown_height <= 0) {
    return false;
  }
  info->width = static_cast<int>(shown_width);
  info->height = static_cast<int>(shown_height);
  info->sar_num = info->sar_den = 1;
  info->color = ColorInfo();
  return true;
}

// Length of the start code at |data[i]|, or 0.
size_t StartCodeAt(const uint8_t* data, size_t size, size_t i) {
  if (i + 3 <= size && data[i] == 0 && data[i + 1] == 0) {
    if (data[i + 2] == 1) {
      return 3;
    }
    if (i + 4 <= size && data[i + 2] == 0 && data[i + 3] == 1) {
      return 4;
    }
  }
  return 0;
}

}  // namespace

bool ParseAccessUnit(const uint8_t* data,
                     size_t size,
                     bool hevc,
                     AccessUnitInfo* info) {
  *info = AccessUnitInfo();
  bool any = false;
  size_t i = 0;
  size_t start_code = 0;
  while (i < size && !(start_code = StartCodeAt(data, size, i))) {
    ++i;
  }
  while (i < size) {
    const size_t begin = i + start_code;
    size_t end = begin;
    while (end < size && !(start_code = StartCodeAt(data, size, end))) {
      ++end;
    }
    i = end;
    if (end <= begin) {
      continue;
    }
    any = true;
    const uint8_t* nal = data + begin;
    const size_t nal_size = end - begin;
    if (hevc) {
      const int type = (nal[0] >> 1) & 0x3f;
      if (type == 33 && !info->has_sps) {
        info->has_sps = ParseHevcSps(nal, nal_size, info);
      } else if (type < 32) {
        // The first slice: IRAP pictures are types 16 to 23.
        info->keyframe = type >= 16 && type <= 23;
        break;
      }
    } else {
      const int type = nal[0] & 0x1f;
      if (type == 7 && !info->has_sps) {
        info->has_sps = ParseH264Sps(nal, nal_size, info);
      } else if (type >= 1 && type <= 5) {
        info->keyframe = type == 5;
        break;
      }
    }
  }
  return any;
}

bool ParseAdtsHeader(const uint8_t* data, size_t size, AdtsHeader* header) {
  // syncword (12 bits), ID, layer (2 bits, always 0), protection_absent.
  if (size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0) {
    return false;
  }
  static const int kRates[13] = {96000, 88200, 64000, 48000, 44100,
                                 32000, 24000, 22050, 16000, 12000,
                                 11025, 8000,  7350};
  const int rate_index = (data[2] >> 2) & 0xf;
  if (rate_index >= 13) {
    return false;
  }
  header->object_type = (data[2] >> 6) + 1;
  header->sample_rate = kRates[rate_index];
  header->channels = ((data[2] & 1) << 2) | (data[3] >> 6);
  // Channel configuration 0 means a program_config_element says; most
  // streams have 1 to 7, and 7 means 7.1.
  if (header->channels == 7) {
    header->channels = 8;
  }
  header->header_size = (data[1] & 1) ? 7 : 9;
  header->frame_length = (static_cast<size_t>(data[3] & 3) << 11) |
                         (static_cast<size_t>(data[4]) << 3) | (data[5] >> 5);
  header->samples = 1024 * ((data[6] & 3) + 1);
  return header->frame_length > header->header_size;
}

bool ParseMp3Header(const uint8_t* data, size_t size, AdtsHeader* header) {
  // Frame sync (11 bits), version (2), layer (2: 1 is Layer III).
  if (size < 4 || data[0] != 0xff || (data[1] & 0xe0) != 0xe0 ||
      ((data[1] >> 1) & 3) != 1) {
    return false;
  }
  const int version = (data[1] >> 3) & 3;  // 0: 2.5, 2: MPEG-2, 3: MPEG-1
  const int bitrate_index = data[2] >> 4;
  const int rate_index = (data[2] >> 2) & 3;
  if (version == 1 || bitrate_index == 0 || bitrate_index == 15 ||
      rate_index == 3) {
    return false;
  }
  // kbit/s.
  static const int kMpeg1Rates[15] = {0,   32,  40,  48,  56,  64,  80, 96,
                                      112, 128, 160, 192, 224, 256, 320};
  static const int kMpeg2Rates[15] = {0,  8,  16, 24,  32,  40,  48, 56,
                                      64, 80, 96, 112, 128, 144, 160};
  static const int kSampleRates[3] = {44100, 48000, 32000};
  const bool mpeg1 = version == 3;
  const int bitrate = (mpeg1 ? kMpeg1Rates : kMpeg2Rates)[bitrate_index] * 1000;
  // MPEG-2 halves the rates, MPEG 2.5 quarters them.
  const int rate_shift = mpeg1 ? 0 : version == 2 ? 1 : 2;
  header->sample_rate = kSampleRates[rate_index] >> rate_shift;
  header->channels = (data[3] >> 6) == 3 ? 1 : 2;
  header->samples = mpeg1 ? 1152 : 576;
  const int length = header->samples / 8 * bitrate / header->sample_rate +
                     ((data[2] >> 1) & 1);  // padding
  header->frame_length = static_cast<size_t>(length);
  header->header_size = 4;
  header->object_type = 0;
  return header->frame_length > header->header_size;
}

bool ParseAc3Header(const uint8_t* data, size_t size, AdtsHeader* header) {
  if (size < 7 || data[0] != 0x0b || data[1] != 0x77) {
    return false;
  }
  static const int kRates[3] = {48000, 44100, 32000};
  // Channels by acmod: 1+1, 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2.
  static const int kChannels[8] = {2, 1, 2, 3, 3, 4, 4, 5};
  const int bsid = data[5] >> 3;
  int rate;
  int acmod;
  bool lfe;
  if (bsid <= 10) {
    // AC-3: crc1, fscod, frmsizecod, bsid, bsmod, acmod, then mixing
    // levels that depend on acmod, then lfeon.
    const int fscod = data[4] >> 6;
    if (fscod == 3) {
      return false;
    }
    rate = kRates[fscod];
    acmod = data[6] >> 5;
    int bit = 3;  // Bits of data[6] used.
    if ((acmod & 1) && acmod != 1) {
      bit += 2;  // cmixlev
    }
    if (acmod & 4) {
      bit += 2;  // surmixlev
    }
    if (acmod == 2) {
      bit += 2;  // dsurmod
    }
    const int byte = 6 + bit / 8;
    lfe = (data[byte] >> (7 - bit % 8)) & 1;
  } else if (bsid <= 16) {
    // E-AC-3: strmtyp, substreamid, frmsiz, fscod, fscod2 or numblkscod,
    // acmod, lfeon.
    const int fscod = data[4] >> 6;
    rate = fscod == 3 ? kRates[(data[4] >> 4) & 3] / 2 : kRates[fscod];
    if (fscod == 3 && ((data[4] >> 4) & 3) == 3) {
      return false;
    }
    acmod = (data[4] >> 1) & 7;
    lfe = data[4] & 1;
  } else {
    return false;
  }
  header->sample_rate = rate;
  header->channels = kChannels[acmod] + (lfe ? 1 : 0);
  return true;
}

}  // namespace lychee
