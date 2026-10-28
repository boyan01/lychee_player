#ifndef LYCHEE_MEDIA_SUBTITLE_IMAGE_H_
#define LYCHEE_MEDIA_SUBTITLE_IMAGE_H_

#include <cstdint>
#include <cstring>
#include <span>

#include "media/subtitle_decoder.h"

namespace lychee {
inline constexpr size_t kMaxSubtitlePngBytes = 4 << 20;
inline constexpr size_t kMaxSubtitleImagePixels = 4 << 20;

// Check the IHDR before a platform decoder allocates its output.
inline bool SubtitlePngSize(std::span<const uint8_t> data, int* w, int* h) {
  constexpr uint8_t kSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (data.size() < 33 || data.size() > kMaxSubtitlePngBytes ||
      std::memcmp(data.data(), kSignature, 8) != 0 ||
      std::memcmp(data.data() + 12, "IHDR", 4) != 0) {
    return false;
  }
  auto get = [&](size_t at) {
    uint32_t value = 0;
    for (size_t i = at; i < at + 4; ++i) {
      value = (value << 8) | data[i];
    }
    return value;
  };
  const uint32_t width = get(16);
  const uint32_t height = get(20);
  if (get(8) != 13 || width == 0 || height == 0 || width > 4096 ||
      height > 4096 || uint64_t{width} * height > kMaxSubtitleImagePixels) {
    return false;
  }
  // Reject duplicate headers and incomplete/concatenated PNGs before the
  // platform decoder sees metadata that disagrees with the checked IHDR.
  bool ended = false;
  bool pixels = false;
  size_t at = 8;
  for (size_t chunks = 0; at < data.size() && chunks < 4096; ++chunks) {
    if (data.size() - at < 12) return false;
    const size_t length = get(at);
    if (length > data.size() - at - 12) return false;
    if (at != 8 && std::memcmp(data.data() + at + 4, "IHDR", 4) == 0)
      return false;
    if (std::memcmp(data.data() + at + 4, "IDAT", 4) == 0) pixels = true;
    if (std::memcmp(data.data() + at + 4, "IEND", 4) == 0) {
      ended = length == 0 && at + 12 == data.size();
      break;
    }
    at += length + 12;
  }
  if (!ended || !pixels) return false;
  *w = static_cast<int>(width);
  *h = static_cast<int>(height);
  return true;
}

// The system PNG decoder, returning top-down premultiplied RGBA pixels.
bool DecodeSubtitlePng(std::span<const uint8_t> data, SubtitleBitmap* image);
}  // namespace lychee
#endif  // LYCHEE_MEDIA_SUBTITLE_IMAGE_H_
