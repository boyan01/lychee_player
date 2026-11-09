#include "media/frame_scaler.h"

#include <cstdint>
#include <vector>

#include "test.h"

namespace lychee {
namespace {

// A |width| x |height| RGBA frame whose pixel (x, y) is (x, y, 7, 255),
// rows padded by 8 bytes.
VideoFrame MakeFrame(int width, int height, std::vector<uint8_t>* pixels) {
  VideoFrame frame;
  frame.storage = VideoFrameStorage::kCpuRgba;
  frame.width = width;
  frame.height = height;
  frame.stride = width * 4 + 8;
  frame.pts = 42;
  pixels->assign(static_cast<size_t>(frame.stride) * height, 0xee);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      uint8_t* pixel = pixels->data() + static_cast<size_t>(y) * frame.stride +
                       static_cast<size_t>(x) * 4;
      pixel[0] = static_cast<uint8_t>(x);
      pixel[1] = static_cast<uint8_t>(y);
      pixel[2] = 7;
      pixel[3] = 255;
    }
  }
  frame.data = pixels->data();
  return frame;
}

const uint8_t* Pixel(const VideoFrame& frame, int x, int y) {
  return frame.data + static_cast<size_t>(y) * frame.stride +
         static_cast<size_t>(x) * 4;
}

// NOLINTBEGIN(bugprone-throwing-static-initialization)
TEST(FrameScaler, FitsKeepingAspect) {
  int w = 0, h = 0;
  FitSize(1920, 1080, 1, 1, 0, 320, 320, &w, &h);
  EXPECT_EQ(w, 320);
  EXPECT_EQ(h, 180);
  // Turned upright first.
  FitSize(1920, 1080, 1, 1, 90, 320, 320, &w, &h);
  EXPECT_EQ(w, 180);
  EXPECT_EQ(h, 320);
  // Anamorphic: 720x576 at 16:15 shows 768 wide.
  FitSize(720, 576, 16, 15, 0, 0, 0, &w, &h);
  EXPECT_EQ(w, 768);
  EXPECT_EQ(h, 576);
  // Never enlarged; one limit alone.
  FitSize(640, 360, 1, 1, 0, 1280, 0, &w, &h);
  EXPECT_EQ(w, 640);
  EXPECT_EQ(h, 360);
  FitSize(640, 360, 1, 1, 0, 0, 90, &w, &h);
  EXPECT_EQ(w, 160);
  EXPECT_EQ(h, 90);
}

TEST(FrameScaler, CopiesAtTheSameSize) {
  std::vector<uint8_t> pixels;
  const VideoFrame frame = MakeFrame(5, 3, &pixels);
  auto scaled = ScaleFrame(frame, 0, 5, 3);
  EXPECT_TRUE(scaled != nullptr);
  EXPECT_EQ(scaled->stride, 20);
  EXPECT_EQ(scaled->pts, 42);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 5; ++x) {
      EXPECT_EQ(Pixel(*scaled, x, y)[0], x);
      EXPECT_EQ(Pixel(*scaled, x, y)[1], y);
      EXPECT_EQ(Pixel(*scaled, x, y)[2], 7);
    }
  }
}

TEST(FrameScaler, AveragesWhenReducing) {
  std::vector<uint8_t> pixels;
  const VideoFrame frame = MakeFrame(64, 32, &pixels);
  auto scaled = ScaleFrame(frame, 0, 16, 8);
  EXPECT_TRUE(scaled != nullptr);
  // Output pixel x covers source 4x..4x+3, centered on 4x + 1.5; a
  // gradient keeps its mean there.
  EXPECT_NEAR(Pixel(*scaled, 5, 3)[0], 21, 1);
  EXPECT_NEAR(Pixel(*scaled, 5, 3)[1], 13, 1);
  EXPECT_EQ(Pixel(*scaled, 5, 3)[2], 7);
  EXPECT_EQ(Pixel(*scaled, 5, 3)[3], 255);
  // The edges clamp rather than fade.
  EXPECT_NEAR(Pixel(*scaled, 0, 0)[2], 7, 0);
  EXPECT_NEAR(Pixel(*scaled, 15, 7)[3], 255, 0);
}

TEST(FrameScaler, Rotates) {
  std::vector<uint8_t> pixels;
  // 3 wide, 2 high.
  const VideoFrame frame = MakeFrame(3, 2, &pixels);
  // Clockwise: the bottom-left source pixel (0, 1) lands top left.
  auto quarter = ScaleFrame(frame, 90, 2, 3);
  EXPECT_EQ(Pixel(*quarter, 0, 0)[0], 0);
  EXPECT_EQ(Pixel(*quarter, 0, 0)[1], 1);
  EXPECT_EQ(Pixel(*quarter, 1, 0)[0], 0);
  EXPECT_EQ(Pixel(*quarter, 1, 0)[1], 0);
  EXPECT_EQ(Pixel(*quarter, 1, 2)[0], 2);
  EXPECT_EQ(Pixel(*quarter, 1, 2)[1], 0);
  auto half = ScaleFrame(frame, 180, 3, 2);
  EXPECT_EQ(Pixel(*half, 0, 0)[0], 2);
  EXPECT_EQ(Pixel(*half, 0, 0)[1], 1);
  // The top-right source pixel (2, 0) lands top left.
  auto three = ScaleFrame(frame, 270, 2, 3);
  EXPECT_EQ(Pixel(*three, 0, 0)[0], 2);
  EXPECT_EQ(Pixel(*three, 0, 0)[1], 0);
  EXPECT_EQ(Pixel(*three, 1, 2)[0], 0);
  EXPECT_EQ(Pixel(*three, 1, 2)[1], 1);
}

TEST(FrameScaler, RefusesOtherStorage) {
  VideoFrame frame;
  frame.storage = VideoFrameStorage::kCVPixelBuffer;
  frame.width = 2;
  frame.height = 2;
  EXPECT_TRUE(ScaleFrame(frame, 0, 1, 1) == nullptr);
}

// NOLINTEND(bugprone-throwing-static-initialization)

}  // namespace
}  // namespace lychee
