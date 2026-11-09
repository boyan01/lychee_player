#include "media/frame_scaler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace lychee {

namespace {

// The source pixels and weights of each output pixel along one axis.
struct Filter {
  std::vector<int> first;
  std::vector<int> count;
  // |count[i]| weights of output pixel i, summing to 1, one after another.
  std::vector<float> weights;
  std::vector<size_t> offset;
};

// A triangle over the source pixels around each output pixel's center,
// at least one source pixel wide on each side, wider by the reduction.
Filter MakeFilter(int source, int output) {
  Filter filter;
  filter.first.resize(output);
  filter.count.resize(output);
  filter.offset.resize(output);
  const double scale = static_cast<double>(source) / output;
  const double radius = std::max(1.0, scale);
  for (int i = 0; i < output; ++i) {
    const double center = (i + 0.5) * scale - 0.5;
    const int begin = static_cast<int>(std::floor(center - radius)) + 1;
    const int end = static_cast<int>(std::ceil(center + radius));
    filter.offset[i] = filter.weights.size();
    // Taps past an edge count for the edge pixel.
    const int first = std::clamp(begin, 0, source - 1);
    const int last = std::clamp(end - 1, 0, source - 1);
    filter.first[i] = first;
    filter.count[i] = last - first + 1;
    filter.weights.resize(filter.weights.size() + filter.count[i], 0.0f);
    float* weights = filter.weights.data() + filter.offset[i];
    double sum = 0;
    for (int tap = begin; tap < end; ++tap) {
      const double weight = 1.0 - std::abs(tap - center) / radius;
      if (weight <= 0) {
        continue;
      }
      weights[std::clamp(tap, 0, source - 1) - first] +=
          static_cast<float>(weight);
      sum += weight;
    }
    if (sum <= 0) {
      weights[std::clamp(static_cast<int>(std::lround(center)), first, last) -
              first] = 1.0f;
      sum = 1;
    }
    for (int k = 0; k < filter.count[i]; ++k) {
      weights[k] = static_cast<float>(weights[k] / sum);
    }
  }
  return filter;
}

}  // namespace

void FitSize(int width,
             int height,
             int sar_num,
             int sar_den,
             int rotation,
             int max_width,
             int max_height,
             int* fit_width,
             int* fit_height) {
  double w = std::max(width, 1);
  double h = std::max(height, 1);
  if (sar_num > 0 && sar_den > 0) {
    w = w * sar_num / sar_den;
  }
  if (rotation % 180 != 0) {
    std::swap(w, h);
  }
  double scale = 1;
  if (max_width > 0) {
    scale = std::min(scale, max_width / w);
  }
  if (max_height > 0) {
    scale = std::min(scale, max_height / h);
  }
  *fit_width = std::max(1, static_cast<int>(std::lround(w * scale)));
  *fit_height = std::max(1, static_cast<int>(std::lround(h * scale)));
}

std::shared_ptr<VideoFrame> ScaleFrame(const VideoFrame& frame,
                                       int rotation,
                                       int width,
                                       int height) {
  if ((frame.storage != VideoFrameStorage::kCpuRgba &&
       frame.storage != VideoFrameStorage::kCpuBgra) ||
      !frame.data || frame.width <= 0 || frame.height <= 0 || width <= 0 ||
      height <= 0) {
    return nullptr;
  }
  rotation = ((rotation % 360) + 360) % 360;
  const bool quarter = rotation % 180 != 0;
  // The scaled picture before it turns.
  const int scaled_width = quarter ? height : width;
  const int scaled_height = quarter ? width : height;
  const Filter horizontal = MakeFilter(frame.width, scaled_width);
  const Filter vertical = MakeFilter(frame.height, scaled_height);

  // Rows scaled horizontally, then columns vertically.
  std::vector<float> rows(static_cast<size_t>(scaled_width) * 4 * frame.height);
  for (int y = 0; y < frame.height; ++y) {
    const uint8_t* source = frame.data + static_cast<size_t>(y) * frame.stride;
    float* out = rows.data() + static_cast<size_t>(y) * scaled_width * 4;
    for (int x = 0; x < scaled_width; ++x) {
      const float* weights = horizontal.weights.data() + horizontal.offset[x];
      const uint8_t* pixel =
          source + static_cast<size_t>(horizontal.first[x]) * 4;
      float sum[4] = {0, 0, 0, 0};
      for (int k = 0; k < horizontal.count[x]; ++k, pixel += 4) {
        for (int c = 0; c < 4; ++c) {
          sum[c] += weights[k] * pixel[c];
        }
      }
      std::memcpy(out + static_cast<size_t>(x) * 4, sum, sizeof(sum));
    }
  }
  std::vector<uint8_t> scaled(static_cast<size_t>(scaled_width) * 4 *
                              scaled_height);
  for (int y = 0; y < scaled_height; ++y) {
    const float* weights = vertical.weights.data() + vertical.offset[y];
    uint8_t* out = scaled.data() + static_cast<size_t>(y) * scaled_width * 4;
    for (int x = 0; x < scaled_width * 4; ++x) {
      float sum = 0;
      for (int k = 0; k < vertical.count[y]; ++k) {
        sum +=
            weights[k] *
            rows[static_cast<size_t>(vertical.first[y] + k) * scaled_width * 4 +
                 x];
      }
      out[x] = static_cast<uint8_t>(std::clamp(std::lround(sum), 0L, 255L));
    }
  }

  auto pixels = std::make_shared<std::vector<uint8_t>>();
  if (rotation == 0) {
    *pixels = std::move(scaled);
  } else {
    pixels->resize(scaled.size());
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        // The scaled pixel that turns into upright (x, y).
        int sx = 0;
        int sy = 0;
        switch (rotation) {
          case 90:
            sx = y, sy = scaled_height - 1 - x;
            break;
          case 180:
            sx = scaled_width - 1 - x, sy = scaled_height - 1 - y;
            break;
          default:  // 270
            sx = scaled_width - 1 - y, sy = x;
            break;
        }
        std::memcpy(
            pixels->data() + (static_cast<size_t>(y) * width + x) * 4,
            scaled.data() + (static_cast<size_t>(sy) * scaled_width + sx) * 4,
            4);
      }
    }
  }
  auto result = std::make_shared<VideoFrame>();
  result->storage = frame.storage;
  result->width = width;
  result->height = height;
  result->pts = frame.pts;
  result->duration = frame.duration;
  result->data = pixels->data();
  result->stride = width * 4;
  result->owner = std::move(pixels);
  return result;
}

}  // namespace lychee
