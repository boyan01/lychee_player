#ifndef LYCHEE_MEDIA_BIT_READER_H_
#define LYCHEE_MEDIA_BIT_READER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lychee {

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

// The first |max| bytes of the NAL unit's payload without emulation
// prevention bytes (00 00 03).
inline std::vector<uint8_t> Rbsp(const uint8_t* nal, size_t size, size_t max) {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size < max ? size : max);
  int zeros = 0;
  for (size_t i = 0; i < size && rbsp.size() < max; ++i) {
    if (zeros >= 2 && nal[i] == 3) {
      zeros = 0;
      continue;
    }
    zeros = nal[i] == 0 ? zeros + 1 : 0;
    rbsp.push_back(nal[i]);
  }
  return rbsp;
}

// Skips an H.264 scaling_list() of |size| coefficients (7.3.2.1.1.1).
inline void SkipH264ScalingList(BitReader* reader, int size) {
  int last = 8;
  int next = 8;
  for (int i = 0; i < size && reader->ok(); ++i) {
    if (next != 0) {
      next = (last + reader->Se() + 256) % 256;
    }
    last = next == 0 ? last : next;
  }
}

}  // namespace lychee

#endif  // LYCHEE_MEDIA_BIT_READER_H_
