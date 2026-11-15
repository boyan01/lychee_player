// Exercises the production CPU presenter against a host window double. This
// checks copies and failure contracts, not Android compositor/device playback.
#include "surface_frame.h"
#include <cassert>
#include <iostream>

using namespace lychee::android;
int main() {
  const uint8_t rgba[] = {1,  2,  3,  4,  5,  6,  7,  8,  99, 99, 99, 99,
                          9,  10, 11, 12, 13, 14, 15, 16, 99, 99, 99, 99,
                          17, 18, 19, 20, 21, 22, 23, 24, 99, 99, 99, 99};
  const int rotations[] = {0, 90, 180, 270, -90, 450};
  const int transforms[] = {0, 4, 3, 7, 7, 4};
  for (int i = 0; i < 6; ++i) {
    ANativeWindow w;
    int width, height;
    WindowSize(rotations[i], 2, 3, &width, &height);
    assert(DrawRgbaFrame(&w, width, height, rgba, 12, 2, 3, rotations[i]) ==
           DrawResult::kPosted);
    assert(w.transform == transforms[i] && w.posts == 1);
    for (int y = 0; y < 3; ++y) {
      for (int x = 0; x < 8; ++x)
        assert(w.pixels[y * 16 + x] == rgba[y * 12 + x]);
      for (int x = 8; x < 16; ++x)
        assert(w.pixels[y * 16 + x] == 0xCD);
    }
  }
  ANativeWindow contiguous;
  const std::vector<uint8_t> packed(48, 0x37);
  assert(DrawRgbaFrame(&contiguous, 2, 3, packed.data(), 16, 2, 3, 0) ==
         DrawResult::kPosted);
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 8; ++x)
      assert(contiguous.pixels[y * 16 + x] == 0x37);
  }
  ANativeWindow w;
  assert(DrawRgbaFrame(nullptr, 2, 3, rgba, 12, 2, 3, 0) ==
         DrawResult::kPending);
  assert(DrawRgbaFrame(&w, 2, 3, rgba, 12, 2, 3, 90) == DrawResult::kPending);
  assert(w.posts == 0);
  for (int failure = 0; failure < 4; ++failure) {
    ANativeWindow broken;
    if (failure == 0) broken.geometry_error = -1;
    if (failure == 1) broken.transform_error = -1;
    if (failure == 2) broken.lock_error = -1;
    if (failure == 3) broken.post_error = -1;
    assert(DrawRgbaFrame(&broken, 2, 3, rgba, 12, 2, 3, 0) ==
           DrawResult::kFailed);
  }
  assert(DrawRgbaFrame(&w, 2, 3, nullptr, 12, 2, 3, 0) == DrawResult::kFailed);
  assert(DrawRgbaFrame(&w, 2, 3, rgba, 4, 2, 3, 0) == DrawResult::kFailed);
  std::cout << "PASS CPU SurfaceView presenter: rotation, stride, resize and "
               "producer failures\n";
}
