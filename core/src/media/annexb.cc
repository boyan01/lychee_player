#include "media/annexb.h"

namespace lychee {

namespace {

constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

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

bool StartsWithStartCode(const std::vector<uint8_t>& data) {
  return StartCodeAt(data.data(), data.size(), 0) != 0;
}

uint32_t ReadBigEndian(const uint8_t* data, int bytes) {
  uint32_t value = 0;
  for (int i = 0; i < bytes; ++i) {
    value = (value << 8) | data[i];
  }
  return value;
}

}  // namespace

bool AnnexBConverter::Initialize(const VideoConfig& config) {
  if (config.codec != VideoCodec::kH264 && config.codec != VideoCodec::kHEVC) {
    return false;
  }
  hevc_ = config.codec == VideoCodec::kHEVC;
  parameter_sets_.clear();
  const std::vector<uint8_t>& extradata = config.extradata;
  if (extradata.empty() || StartsWithStartCode(extradata)) {
    // MPEG-TS and raw streams: parameter sets travel in band.
    length_size_ = 0;
    return ParseAnnexB(extradata);
  }
  return hevc_ ? ParseHvcC(extradata) : ParseAvcC(extradata);
}

bool AnnexBConverter::ParseAvcC(const std::vector<uint8_t>& data) {
  // configurationVersion, profile, compatibility, level,
  // lengthSizeMinusOne, numOfSequenceParameterSets.
  if (data.size() < 7 || data[0] != 1) {
    return false;
  }
  length_size_ = (data[4] & 3) + 1;
  size_t pos = 5;
  for (int list = 0; list < 2; ++list) {
    if (pos >= data.size()) {
      return false;
    }
    // SPS count has 5 bits, PPS count 8.
    const int count = list == 0 ? data[pos] & 0x1f : data[pos];
    ++pos;
    for (int i = 0; i < count; ++i) {
      if (pos + 2 > data.size()) {
        return false;
      }
      const size_t length = ReadBigEndian(&data[pos], 2);
      pos += 2;
      if (pos + length > data.size()) {
        return false;
      }
      parameter_sets_.emplace_back(data.begin() + pos,
                                   data.begin() + pos + length);
      pos += length;
    }
  }
  return true;
}

bool AnnexBConverter::ParseHvcC(const std::vector<uint8_t>& data) {
  // 22 bytes of profile and format fields; lengthSizeMinusOne is in the
  // low bits of byte 21, numOfArrays follows.
  if (data.size() < 23 || data[0] != 1) {
    return false;
  }
  length_size_ = (data[21] & 3) + 1;
  const int arrays = data[22];
  size_t pos = 23;
  for (int a = 0; a < arrays; ++a) {
    if (pos + 3 > data.size()) {
      return false;
    }
    const int count = static_cast<int>(ReadBigEndian(&data[pos + 1], 2));
    pos += 3;
    for (int i = 0; i < count; ++i) {
      if (pos + 2 > data.size()) {
        return false;
      }
      const size_t length = ReadBigEndian(&data[pos], 2);
      pos += 2;
      if (pos + length > data.size()) {
        return false;
      }
      parameter_sets_.emplace_back(data.begin() + pos,
                                   data.begin() + pos + length);
      pos += length;
    }
  }
  return true;
}

bool AnnexBConverter::ParseAnnexB(const std::vector<uint8_t>& data) {
  const uint8_t* bytes = data.data();
  const size_t size = data.size();
  size_t begin = size;
  for (size_t i = 0; i < size;) {
    const size_t length = StartCodeAt(bytes, size, i);
    if (!length) {
      ++i;
      continue;
    }
    if (begin < i) {
      parameter_sets_.emplace_back(bytes + begin, bytes + i);
    }
    begin = i + length;
    i = begin;
  }
  if (begin < size) {
    parameter_sets_.emplace_back(bytes + begin, bytes + size);
  }
  return true;
}

int AnnexBConverter::NalType(const std::vector<uint8_t>& nal) const {
  if (nal.empty()) {
    return -1;
  }
  return hevc_ ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
}

AnnexBConverter::Picture AnnexBConverter::Classify(const uint8_t* data,
                                                   size_t size) const {
  if (length_size_ == 0) {
    return Picture::kUnknown;
  }
  size_t pos = 0;
  while (pos + length_size_ <= size) {
    const size_t length = ReadBigEndian(data + pos, length_size_);
    pos += length_size_;
    if (length > size - pos) {
      return Picture::kUnknown;
    }
    const Picture picture = ClassifyNal(data + pos, length, hevc_);
    if (picture != Picture::kUnknown) {
      return picture;
    }
    pos += length;
  }
  return Picture::kUnknown;
}

// static
AnnexBConverter::Picture AnnexBConverter::ClassifyNal(const uint8_t* nal,
                                                      size_t size,
                                                      bool hevc) {
  if (size == 0) {
    return Picture::kUnknown;
  }
  if (hevc) {
    // ITU-T H.265 table 7-1: 0-9 are trailing and leading pictures,
    // 16-23 IRAP (BLA, IDR, CRA, and reserved IRAP types).
    const int type = (nal[0] >> 1) & 0x3f;
    if (type >= 16 && type <= 23) {
      return Picture::kKeyframe;
    }
    return type <= 9 ? Picture::kInter : Picture::kUnknown;
  }
  const int type = nal[0] & 0x1f;
  if (type == 5) {
    return Picture::kKeyframe;
  }
  if (type != 1 && type != 2) {
    return Picture::kUnknown;
  }
  // The slice header starts first_mb_in_slice, slice_type (both ue(v);
  // ITU-T H.264 7.3.3), with emulation prevention bytes left out.
  size_t byte = 1;
  int bit = 7;
  int zeros = 0;
  auto next_bit = [&]() -> int {
    if (byte >= size) {
      return -1;
    }
    if (bit == 7 && zeros >= 2 && nal[byte] == 3) {
      zeros = 0;
      if (++byte >= size) {
        return -1;
      }
    }
    const int value = (nal[byte] >> bit) & 1;
    if (bit-- == 0) {
      zeros = nal[byte] == 0 ? zeros + 1 : 0;
      bit = 7;
      byte++;
    }
    return value;
  };
  auto ue = [&]() -> int64_t {
    int leading = 0;
    for (int b = next_bit(); b != 1; b = next_bit()) {
      if (b < 0 || ++leading > 31) {
        return -1;
      }
    }
    int64_t value = 0;
    for (int i = 0; i < leading; ++i) {
      const int b = next_bit();
      if (b < 0) {
        return -1;
      }
      value = (value << 1) | b;
    }
    return (int64_t{1} << leading) - 1 + value;
  };
  if (ue() < 0) {
    return Picture::kUnknown;
  }
  const int64_t slice_type = ue();
  if (slice_type < 0 || slice_type > 9) {
    return Picture::kUnknown;
  }
  // 2 and 7 are I, 4 and 9 SI.
  return slice_type % 5 == 2 || slice_type % 5 == 4 ? Picture::kIntra
                                                    : Picture::kInter;
}

// static
void AnnexBConverter::AppendNal(const uint8_t* nal,
                                size_t size,
                                std::vector<uint8_t>* out) {
  out->insert(out->end(), kStartCode, kStartCode + sizeof(kStartCode));
  out->insert(out->end(), nal, nal + size);
}

bool AnnexBConverter::Convert(const uint8_t* data,
                              size_t size,
                              bool keyframe,
                              std::vector<uint8_t>* out,
                              bool four_byte_start_codes) const {
  if (keyframe) {
    for (const auto& nal : parameter_sets_) {
      AppendNal(nal.data(), nal.size(), out);
    }
  }
  if (length_size_ == 0) {
    if (!four_byte_start_codes) {
      out->insert(out->end(), data, data + size);
      return true;
    }
    size_t begin = size;
    for (size_t i = 0; i < size;) {
      const size_t length = StartCodeAt(data, size, i);
      if (!length) {
        ++i;
        continue;
      }
      if (begin < i) {
        AppendNal(data + begin, i - begin, out);
      }
      begin = i + length;
      i = begin;
    }
    if (begin < size) {
      AppendNal(data + begin, size - begin, out);
    }
    return size == 0 || begin < size;
  }
  size_t pos = 0;
  while (pos + length_size_ <= size) {
    const size_t length = ReadBigEndian(data + pos, length_size_);
    pos += length_size_;
    if (length > size - pos) {
      return false;
    }
    AppendNal(data + pos, length, out);
    pos += length;
  }
  return pos == size;
}

}  // namespace lychee
