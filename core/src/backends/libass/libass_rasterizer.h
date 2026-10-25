#ifndef LYCHEE_BACKENDS_LIBASS_LIBASS_RASTERIZER_H_
#define LYCHEE_BACKENDS_LIBASS_LIBASS_RASTERIZER_H_

#include <mutex>

#include "media/subtitle_decoder.h"

extern "C" {
#include <ass/ass.h>
}

namespace lychee {

// ASS/SSA through libass: styles, positions, fonts the media carries and
// animations. Each frame's glyph, outline and shadow images are blended
// into one premultiplied RGBA picture.
class LibassRasterizer : public SubtitleRasterizer {
 public:
  LibassRasterizer();
  ~LibassRasterizer() override;

  bool Initialize(const SubtitleConfig& config,
                  const std::vector<SubtitleFont>& fonts,
                  std::string* error) override;
  void Decode(const EncodedPacket& packet) override;
  uint64_t EventsAt(int64_t media_time) override;
  bool Render(int64_t media_time,
              int width,
              int height,
              int video_width,
              int video_height,
              std::vector<SubtitleBitmap>* bitmaps) override;

 private:
  std::mutex mutex_;
  ASS_Library* library_ = nullptr;
  ASS_Renderer* renderer_ = nullptr;
  ASS_Track* track_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int video_width_ = 0;
  int video_height_ = 0;
  bool rendered_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_LIBASS_LIBASS_RASTERIZER_H_
