#define LOG_TAG "pgs"

#include "media/pgs_subtitle_decoder.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/log.h"
#include "media/subtitle_image.h"

namespace lychee {

namespace {

enum SegmentType : uint8_t {
  kPalette = 0x14,
  kObject = 0x15,
  kComposition = 0x16,
  kWindow = 0x17,
  kEnd = 0x80,
};

// What a stream may make the decoder hold: the format allows 64 objects
// and 8 palettes an epoch. An object's run-length data takes up to two
// bytes a pixel and two a row.
constexpr size_t kMaxObjects = 64;
constexpr size_t kMaxPalettes = 8;
constexpr size_t kMaxObjectBytes =
    2 * kMaxSubtitleImagePixels + size_t{2} * 4096;
constexpr size_t kMaxHeldBytes = 4 * kMaxObjectBytes;
constexpr int kMaxSide = 4096;

int Get16(const uint8_t* data) {
  return (data[0] << 8) | data[1];
}

}  // namespace

bool PgsSubtitleDecoder::Initialize(const SubtitleConfig& config,
                                    std::string* error) {
  if (config.codec_name != "hdmv_pgs_subtitle") {
    *error = "pgs: not PGS: " + config.codec_name;
    return false;
  }
  return true;
}

// static
bool PgsSubtitleDecoder::DecodeRle(const uint8_t* data,
                                   size_t size,
                                   int width,
                                   int height,
                                   std::vector<uint8_t>* indices) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  indices->assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
  int x = 0;
  int y = 0;
  size_t i = 0;
  while (i < size && y < height) {
    // A byte other than 0 is one pixel of that colour; 0 starts a run, or
    // with 0 after it ends the row.
    int run = 1;
    uint8_t color = data[i++];
    if (color == 0) {
      if (i >= size) {
        break;
      }
      const uint8_t flags = data[i++];
      if (flags == 0) {
        x = 0;
        ++y;
        continue;
      }
      run = flags & 0x3f;
      if (flags & 0x40) {
        if (i >= size) {
          break;
        }
        run = (run << 8) | data[i++];
      }
      if (flags & 0x80) {
        if (i >= size) {
          break;
        }
        color = data[i++];
      }
    }
    const int n = std::min(run, width - x);
    if (n > 0) {
      std::fill_n(indices->begin() + static_cast<ptrdiff_t>(y) * width + x, n,
                  color);
      x += n;
    }
  }
  return true;
}

// static
uint32_t PgsSubtitleDecoder::ToRgba(uint8_t y,
                                    uint8_t cr,
                                    uint8_t cb,
                                    uint8_t alpha,
                                    int canvas_height) {
  // The library's convention for untagged video, which FFmpeg's decoder
  // follows for PGS too.
  const bool bt709 = canvas_height > 576;
  const double kr = bt709 ? 0.2126 : 0.299;
  const double kb = bt709 ? 0.0722 : 0.114;
  const double luma = (y - 16) / 219.0;
  const double pr = (cr - 128) / 224.0;
  const double pb = (cb - 128) / 224.0;
  const double r = luma + 2 * (1 - kr) * pr;
  const double b = luma + 2 * (1 - kb) * pb;
  const double g = (luma - kr * r - kb * b) / (1 - kr - kb);
  auto channel = [alpha](double value) {
    const double clamped = std::clamp(value, 0.0, 1.0);
    return static_cast<uint32_t>(std::lround(clamped * alpha));
  };
  return channel(r) | (channel(g) << 8) | (channel(b) << 16) |
         (static_cast<uint32_t>(alpha) << 24);
}

bool PgsSubtitleDecoder::Decode(const EncodedPacket& packet,
                                std::vector<SubtitleCue>* cues) {
  if (packet.end_of_stream || !packet.buffer) {
    return true;
  }
  const uint8_t* data = packet.buffer->data();
  size_t size = packet.buffer->size();
  while (size >= 3) {
    // A .sup file's header: "PG", presentation and decoding times.
    if (data[0] == 'P' && data[1] == 'G' && size >= 13) {
      data += 10;
      size -= 10;
    }
    const uint8_t type = data[0];
    const size_t length = std::min<size_t>(Get16(data + 1), size - 3);
    const uint8_t* payload = data + 3;
    data += 3 + length;
    size -= 3 + length;
    switch (type) {
      case kComposition:
        Composition(payload, length, packet);
        break;
      case kPalette:
        PaletteSegment(payload, length);
        break;
      case kObject:
        ObjectSegment(payload, length);
        break;
      case kEnd:
        if (composing_) {
          if (duration_ <= 0 && packet.duration > 0 && packet.pts == start_) {
            duration_ = packet.duration;
          }
          cues->push_back(End());
        }
        break;
      case kWindow:
      default:
        break;
    }
  }
  return true;
}

void PgsSubtitleDecoder::Composition(const uint8_t* data,
                                     size_t size,
                                     const EncodedPacket& at) {
  // Width, height, frame rate, number, state, palette update flag,
  // palette id, object count; then each object's placement.
  if (size < 11 || at.pts == kNoTimestamp) {
    composing_ = false;
    return;
  }
  const uint8_t state = data[7];
  // The start of an epoch: what came before is not used again.
  if (state & 0x80) {
    objects_.clear();
    palettes_.clear();
  }
  composing_ = true;
  start_ = at.pts;
  duration_ = at.duration;
  width_ = Get16(data);
  height_ = Get16(data + 2);
  palette_id_ = data[9];
  placements_.clear();
  const int count = data[10];
  size_t offset = 11;
  for (int i = 0; i < count && offset + 8 <= size; ++i) {
    Placement placement;
    placement.object_id = Get16(data + offset);
    placement.cropped = (data[offset + 3] & 0x80) != 0;
    placement.x = Get16(data + offset + 4);
    placement.y = Get16(data + offset + 6);
    offset += 8;
    if (placement.cropped) {
      if (offset + 8 > size) {
        break;
      }
      placement.crop_x = Get16(data + offset);
      placement.crop_y = Get16(data + offset + 2);
      placement.crop_width = Get16(data + offset + 4);
      placement.crop_height = Get16(data + offset + 6);
      offset += 8;
    }
    placements_.push_back(placement);
  }
}

void PgsSubtitleDecoder::PaletteSegment(const uint8_t* data, size_t size) {
  // Id, version, then entries of index, Y, Cr, Cb and alpha.
  if (size < 2) {
    return;
  }
  auto found = palettes_.find(data[0]);
  if (found == palettes_.end()) {
    if (palettes_.size() >= kMaxPalettes) {
      return;
    }
    found = palettes_.emplace(data[0], Palette{}).first;
  }
  for (size_t offset = 2; offset + 5 <= size; offset += 5) {
    found->second[data[offset]] = {data[offset + 1], data[offset + 2],
                                   data[offset + 3], data[offset + 4]};
  }
}

void PgsSubtitleDecoder::ObjectSegment(const uint8_t* data, size_t size) {
  // Id, version, sequence flags (0x80 first fragment, 0x40 last); the
  // first holds the data's length (with the size after it) and the size.
  if (size < 4) {
    return;
  }
  const int id = Get16(data);
  const bool first = (data[3] & 0x80) != 0;
  data += 4;
  size -= 4;
  size_t held = 0;
  for (const auto& [other, object] : objects_) {
    if (other != id) {
      held += object.data.size();
    }
  }
  if (first) {
    if (size < 7) {
      return;
    }
    const size_t length =
        (static_cast<size_t>(data[0]) << 16) | (data[1] << 8) | data[2];
    Object object;
    object.width = Get16(data + 3);
    object.height = Get16(data + 5);
    object.expected = length >= 4 ? length - 4 : 0;
    data += 7;
    size -= 7;
    if (object.width <= 0 || object.height <= 0 || object.width > kMaxSide ||
        object.height > kMaxSide ||
        static_cast<size_t>(object.width) * object.height >
            kMaxSubtitleImagePixels ||
        object.expected > kMaxObjectBytes ||
        held + object.expected > kMaxHeldBytes ||
        (!objects_.count(id) && objects_.size() >= kMaxObjects)) {
      objects_.erase(id);
      return;
    }
    object.data.reserve(object.expected);
    objects_[id] = std::move(object);
  }
  auto found = objects_.find(id);
  if (found == objects_.end()) {
    return;
  }
  Object& object = found->second;
  const size_t room = object.expected - object.data.size();
  object.data.insert(object.data.end(), data, data + std::min(size, room));
}

SubtitleCue PgsSubtitleDecoder::End() {
  composing_ = false;
  SubtitleCue cue;
  cue.start = start_;
  if (duration_ > 0) {
    cue.end = start_ + duration_;
  }
  cue.canvas_width = width_;
  cue.canvas_height = height_;
  const auto palette = palettes_.find(palette_id_);
  if (palette == palettes_.end()) {
    // Nothing to draw with: an empty cue, which clears the screen.
    return cue;
  }
  std::array<uint32_t, 256> colors{};
  for (size_t i = 0; i < colors.size(); ++i) {
    const auto& entry = palette->second[i];
    colors[i] = ToRgba(entry[0], entry[1], entry[2], entry[3], height_);
  }
  std::vector<SubtitleBitmap> bitmaps;
  std::vector<uint8_t> indices;
  size_t pixels = 0;
  for (const Placement& placement : placements_) {
    const auto found = objects_.find(placement.object_id);
    if (found == objects_.end()) {
      continue;
    }
    const Object& object = found->second;
    if (!DecodeRle(object.data.data(), object.data.size(), object.width,
                   object.height, &indices)) {
      continue;
    }
    int left = 0;
    int top = 0;
    int width = object.width;
    int height = object.height;
    if (placement.cropped) {
      left = std::min(placement.crop_x, object.width);
      top = std::min(placement.crop_y, object.height);
      width = std::min(placement.crop_width, object.width - left);
      height = std::min(placement.crop_height, object.height - top);
    }
    if (width <= 0 || height <= 0 ||
        pixels + static_cast<size_t>(width) * height >
            kMaxSubtitleImagePixels) {
      continue;
    }
    pixels += static_cast<size_t>(width) * height;
    SubtitleBitmap bitmap;
    bitmap.x = placement.x;
    bitmap.y = placement.y;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.rgba.resize(static_cast<size_t>(width) * height * 4);
    uint8_t* out = bitmap.rgba.data();
    for (int row = 0; row < height; ++row) {
      const uint8_t* in =
          indices.data() + static_cast<size_t>(top + row) * object.width + left;
      for (int column = 0; column < width; ++column, out += 4) {
        const uint32_t rgba = colors[in[column]];
        out[0] = static_cast<uint8_t>(rgba);
        out[1] = static_cast<uint8_t>(rgba >> 8);
        out[2] = static_cast<uint8_t>(rgba >> 16);
        out[3] = static_cast<uint8_t>(rgba >> 24);
      }
    }
    bitmaps.push_back(std::move(bitmap));
  }
  if (!bitmaps.empty()) {
    cue.bitmaps =
        std::make_shared<const std::vector<SubtitleBitmap>>(std::move(bitmaps));
  }
  return cue;
}

void PgsSubtitleDecoder::Reset() {
  composing_ = false;
  placements_.clear();
  palettes_.clear();
  objects_.clear();
}

void PgsSubtitleDecoder::Flush() {
  // After a seek a composition may refer to objects of an epoch that
  // started before it; the next epoch start or acquisition point has all.
  Reset();
}

}  // namespace lychee
