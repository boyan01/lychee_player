#include "media/subtitle_decoder.h"

#include <cstring>
#include <limits>

namespace lychee {
namespace {
constexpr size_t kMaxCueBytes = 20 << 20;
void Put(std::vector<uint8_t>* data, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    data->push_back(static_cast<uint8_t>(value >> shift));
  }
}
}  // namespace

std::shared_ptr<PacketBuffer> PackSubtitleCue(const SubtitleCue& cue) {
  size_t size = 16 + cue.text.size();
  if (cue.text.size() > (4 << 20) || cue.canvas_width < 0 ||
      cue.canvas_height < 0 || cue.canvas_width > 16384 ||
      cue.canvas_height > 16384 || (cue.bitmaps && cue.bitmaps->size() > 64)) {
    return nullptr;
  }
  if (cue.bitmaps) {
    for (const SubtitleBitmap& bitmap : *cue.bitmaps) {
      if (bitmap.x < 0 || bitmap.y < 0 || bitmap.x > 16384 ||
          bitmap.y > 16384 || bitmap.width <= 0 || bitmap.height <= 0 ||
          bitmap.width > 4096 || bitmap.height > 4096 ||
          bitmap.rgba.size() !=
              static_cast<size_t>(bitmap.width) * bitmap.height * 4 ||
          size > kMaxCueBytes - 16 ||
          bitmap.rgba.size() > kMaxCueBytes - size - 16)
        return nullptr;
      size += 16 + bitmap.rgba.size();
    }
  }
  std::vector<uint8_t> data;
  data.reserve(size);
  Put(&data, static_cast<uint32_t>(cue.canvas_width));
  Put(&data, static_cast<uint32_t>(cue.canvas_height));
  Put(&data, static_cast<uint32_t>(cue.text.size()));
  Put(&data, cue.bitmaps ? static_cast<uint32_t>(cue.bitmaps->size()) : 0);
  data.insert(data.end(), cue.text.begin(), cue.text.end());
  if (cue.bitmaps) {
    for (const SubtitleBitmap& bitmap : *cue.bitmaps) {
      Put(&data, static_cast<uint32_t>(bitmap.x));
      Put(&data, static_cast<uint32_t>(bitmap.y));
      Put(&data, static_cast<uint32_t>(bitmap.width));
      Put(&data, static_cast<uint32_t>(bitmap.height));
      data.insert(data.end(), bitmap.rgba.begin(), bitmap.rgba.end());
    }
  }
  if (data.size() > kMaxCueBytes) {
    return nullptr;
  }
  return PacketBuffer::Copy(data.data(), data.size());
}

bool UnpackSubtitleCue(const EncodedPacket& packet, SubtitleCue* cue) {
  if (!packet.buffer || packet.size() < 16 || packet.size() > kMaxCueBytes ||
      (packet.duration > 0 && packet.pts > INT64_MAX - packet.duration)) {
    return false;
  }
  size_t at = 0;
  auto get = [&]() {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value = (value << 8) | packet.buffer->data()[at++];
    }
    return value;
  };
  const uint32_t width = get();
  const uint32_t height = get();
  const uint32_t text_size = get();
  const uint32_t count = get();
  if (width > 16384 || height > 16384 || count > 64 || text_size > (4 << 20) ||
      text_size > packet.size() - at) {
    return false;
  }
  SubtitleCue parsed;
  parsed.start = packet.pts;
  parsed.end =
      packet.duration > 0 ? packet.pts + packet.duration : kNoTimestamp;
  parsed.canvas_width = static_cast<int>(width);
  parsed.canvas_height = static_cast<int>(height);
  parsed.text.assign(reinterpret_cast<const char*>(packet.buffer->data() + at),
                     text_size);
  at += text_size;
  std::vector<SubtitleBitmap> bitmaps;
  for (uint32_t i = 0; i < count; ++i) {
    if (packet.size() - at < 16) {
      return false;
    }
    const uint32_t x = get();
    const uint32_t y = get();
    const uint32_t w = get();
    const uint32_t h = get();
    if (x > 16384 || y > 16384 || w == 0 || h == 0 || w > 4096 || h > 4096 ||
        uint64_t{w} * h * 4 > packet.size() - at) {
      return false;
    }
    SubtitleBitmap bitmap;
    bitmap.x = static_cast<int>(x);
    bitmap.y = static_cast<int>(y);
    bitmap.width = static_cast<int>(w);
    bitmap.height = static_cast<int>(h);
    const size_t size = size_t{w} * h * 4;
    bitmap.rgba.assign(packet.buffer->data() + at,
                       packet.buffer->data() + at + size);
    at += size;
    bitmaps.push_back(std::move(bitmap));
  }
  if (at != packet.size()) {
    return false;
  }
  if (!bitmaps.empty()) {
    parsed.bitmaps =
        std::make_shared<const std::vector<SubtitleBitmap>>(std::move(bitmaps));
  }
  *cue = std::move(parsed);
  return true;
}
}  // namespace lychee
