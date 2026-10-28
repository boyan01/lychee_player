#include "media/subtitle_image.h"

#include <png.h>

namespace lychee {
bool DecodeSubtitlePng(std::span<const uint8_t> bytes, SubtitleBitmap* bitmap) {
  int width = 0;
  int height = 0;
  if (!SubtitlePngSize(bytes, &width, &height)) {
    return false;
  }
  png_image png = {};
  png.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&png, bytes.data(), bytes.size())) {
    png_image_free(&png);
    return false;
  }
  png.format = PNG_FORMAT_RGBA;
  SubtitleBitmap image;
  image.width = width;
  image.height = height;
  image.rgba.resize(static_cast<size_t>(width) * height * 4);
  const bool ok =
      png_image_finish_read(&png, nullptr, image.rgba.data(), 0, nullptr) != 0;
  png_image_free(&png);
  if (!ok) {
    return false;
  }
  for (size_t i = 0; i < image.rgba.size(); i += 4) {
    const uint32_t alpha = image.rgba[i + 3];
    for (size_t c = 0; c < 3; ++c) {
      image.rgba[i + c] =
          static_cast<uint8_t>((image.rgba[i + c] * alpha + 127) / 255);
    }
  }
  *bitmap = std::move(image);
  return true;
}
}  // namespace lychee
