#define LOG_TAG "thumbnail"

#include "backends/android/surface_frame_reader.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "base/log.h"
#include "base/time.h"
#include "media/factory.h"

namespace lychee {

namespace {

void DiscardPending(AImageReader* reader) {
  AImage* image = nullptr;
  while (AImageReader_acquireNextImage(reader, &image) == AMEDIA_OK) {
    AImage_delete(image);
  }
}

// Luma weights of the matrix (ITU-T H.273 code points); untagged video
// follows the library's convention: BT.709 above 576 lines, else BT.601.
void LumaWeights(const VideoConfig& config, int height, float* kr, float* kb) {
  int matrix = config.color.matrix;
  if (matrix != 1 && matrix != 5 && matrix != 6 && matrix != 9 &&
      matrix != 10) {
    matrix = height > 576 ? 1 : 6;
  }
  switch (matrix) {
    case 1:
      *kr = 0.2126f, *kb = 0.0722f;
      break;
    case 9:
    case 10:
      *kr = 0.2627f, *kb = 0.0593f;
      break;
    default:
      *kr = 0.299f, *kb = 0.114f;
      break;
  }
}

uint8_t Clamp(float value) {
  return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f));
}

}  // namespace

std::shared_ptr<SurfaceFrameReader> SurfaceFrameReader::Create(int width,
                                                               int height) {
  AImageReader* reader = nullptr;
  if (AImageReader_newWithUsage(
          std::max(width, 1), std::max(height, 1), AIMAGE_FORMAT_YUV_420_888,
          AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 2, &reader) != AMEDIA_OK) {
    LOGW("cannot create a YUV ImageReader");
    return nullptr;
  }
  return std::shared_ptr<SurfaceFrameReader>(new SurfaceFrameReader(reader));
}

SurfaceFrameReader::SurfaceFrameReader(AImageReader* reader)
    : reader_(reader) {}

SurfaceFrameReader::~SurfaceFrameReader() {
  AImageReader_delete(reader_);
}

std::shared_ptr<void> SurfaceFrameReader::surface() const {
  ANativeWindow* window = nullptr;
  AImageReader_getWindow(reader_, &window);
  return RetainVideoSurface(window);
}

std::shared_ptr<VideoFrame> SurfaceFrameReader::Read(
    const VideoFrame& frame,
    const VideoConfig& config) {
  if (!frame.present) {
    return nullptr;
  }
  DiscardPending(reader_);
  frame.present(MonotonicMicros());
  AImage* image = nullptr;
  const int64_t deadline = MonotonicMicros() + kMicrosPerSecond;
  while (AImageReader_acquireNextImage(reader_, &image) != AMEDIA_OK) {
    if (MonotonicMicros() >= deadline) {
      LOGW("no picture from the decoder's window");
      return nullptr;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  std::unique_ptr<AImage, void (*)(AImage*)> owned(image, AImage_delete);
  AImageCropRect crop = {};
  int32_t width = 0;
  int32_t height = 0;
  if (AImage_getWidth(image, &width) != AMEDIA_OK ||
      AImage_getHeight(image, &height) != AMEDIA_OK) {
    return nullptr;
  }
  if (AImage_getCropRect(image, &crop) != AMEDIA_OK ||
      crop.right <= crop.left || crop.bottom <= crop.top) {
    crop = {0, 0, width, height};
  }
  crop.right = std::min(crop.right, width);
  crop.bottom = std::min(crop.bottom, height);
  uint8_t* planes[3] = {};
  int32_t row_strides[3] = {};
  int32_t pixel_strides[3] = {};
  for (int i = 0; i < 3; ++i) {
    int length = 0;
    if (AImage_getPlaneData(image, i, &planes[i], &length) != AMEDIA_OK ||
        AImage_getPlaneRowStride(image, i, &row_strides[i]) != AMEDIA_OK ||
        AImage_getPlanePixelStride(image, i, &pixel_strides[i]) != AMEDIA_OK) {
      LOGW("cannot read the decoder's picture");
      return nullptr;
    }
  }
  const int out_width = crop.right - crop.left;
  const int out_height = crop.bottom - crop.top;
  if (out_width <= 0 || out_height <= 0) {
    return nullptr;
  }
  float kr = 0;
  float kb = 0;
  LumaWeights(config, out_height, &kr, &kb);
  const float kg = 1 - kr - kb;
  const bool full = config.color.full_range;
  const float y_scale = full ? 1.0f : 255.0f / 219.0f;
  const float y_offset = full ? 0.0f : 16.0f;
  const float c_scale = full ? 1.0f : 255.0f / 224.0f;
  auto pixels = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(out_width) * out_height * 4);
  for (int y = 0; y < out_height; ++y) {
    const int sy = crop.top + y;
    const uint8_t* luma = planes[0] + static_cast<size_t>(sy) * row_strides[0];
    const uint8_t* cb =
        planes[1] + static_cast<size_t>(sy / 2) * row_strides[1];
    const uint8_t* cr =
        planes[2] + static_cast<size_t>(sy / 2) * row_strides[2];
    uint8_t* out = pixels->data() + static_cast<size_t>(y) * out_width * 4;
    for (int x = 0; x < out_width; ++x) {
      const int sx = crop.left + x;
      const float l =
          (luma[static_cast<size_t>(sx) * pixel_strides[0]] - y_offset) *
          y_scale;
      const float u =
          (cb[static_cast<size_t>(sx / 2) * pixel_strides[1]] - 128.0f) *
          c_scale;
      const float v =
          (cr[static_cast<size_t>(sx / 2) * pixel_strides[2]] - 128.0f) *
          c_scale;
      out[0] = Clamp(l + 2 * (1 - kr) * v);
      out[1] = Clamp(l - (2 * kb * (1 - kb) * u + 2 * kr * (1 - kr) * v) / kg);
      out[2] = Clamp(l + 2 * (1 - kb) * u);
      out[3] = 255;
      out += 4;
    }
  }
  auto result = std::make_shared<VideoFrame>();
  result->storage = VideoFrameStorage::kCpuRgba;
  result->width = out_width;
  result->height = out_height;
  result->pts = frame.pts;
  result->duration = frame.duration;
  result->data = pixels->data();
  result->stride = out_width * 4;
  result->owner = std::move(pixels);
  return result;
}

}  // namespace lychee
