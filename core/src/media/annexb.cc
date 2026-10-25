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
                              std::vector<uint8_t>* out) const {
  if (keyframe) {
    for (const auto& nal : parameter_sets_) {
      AppendNal(nal.data(), nal.size(), out);
    }
  }
  if (length_size_ == 0) {
    out->insert(out->end(), data, data + size);
    return true;
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
