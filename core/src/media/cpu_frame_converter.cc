#include "media/cpu_frame_converter.h"

#include <cstring>
#include <vector>

namespace lychee {

namespace {

bool IsCpu(VideoFrameStorage storage) {
  return storage == VideoFrameStorage::kCpuBgra ||
         storage == VideoFrameStorage::kCpuRgba;
}

}  // namespace

std::shared_ptr<VideoFrame> CpuFrameConverter::Convert(
    const std::shared_ptr<VideoFrame>& frame,
    uint32_t accepted) {
  if (accepted & static_cast<uint32_t>(frame->storage)) {
    return frame;
  }
  if (!IsCpu(frame->storage)) {
    return nullptr;
  }
  const VideoFrameStorage target = frame->storage == VideoFrameStorage::kCpuBgra
                                       ? VideoFrameStorage::kCpuRgba
                                       : VideoFrameStorage::kCpuBgra;
  if (!(accepted & static_cast<uint32_t>(target))) {
    return nullptr;
  }
  const int stride = frame->width * 4;
  auto pixels = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(stride) * frame->height);
  for (int y = 0; y < frame->height; ++y) {
    const uint8_t* source = frame->data + static_cast<size_t>(y) * frame->stride;
    uint8_t* destination = pixels->data() + static_cast<size_t>(y) * stride;
    // A pixel at a time, not a byte: a quarter of the memory accesses where
    // nothing vectorizes this (debug and ThreadSanitizer builds).
    for (int x = 0; x < frame->width; ++x) {
      uint32_t pixel;
      std::memcpy(&pixel, source, 4);
      pixel = (pixel & 0xff00ff00u) | ((pixel & 0xffu) << 16) |
              ((pixel >> 16) & 0xffu);
      std::memcpy(destination, &pixel, 4);
      source += 4;
      destination += 4;
    }
  }
  auto result = std::make_shared<VideoFrame>(*frame);
  result->storage = target;
  result->data = pixels->data();
  result->stride = stride;
  result->owner = std::move(pixels);
  return result;
}

}  // namespace lychee
