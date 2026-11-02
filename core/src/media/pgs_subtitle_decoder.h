#ifndef LYCHEE_MEDIA_PGS_SUBTITLE_DECODER_H_
#define LYCHEE_MEDIA_PGS_SUBTITLE_DECODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "media/subtitle_decoder.h"

namespace lychee {

// PGS (Blu-ray presentation graphics, "hdmv_pgs_subtitle") without FFmpeg.
// A packet holds segments, each a type byte and a 16-bit length (a .sup
// file's "PG" header before each is skipped): a composition (PCS) places
// objects on the canvas, palettes (PDS) give their colours, objects (ODS)
// their run-length coded pixels, possibly in fragments, and END shows what
// the composition says. Objects and palettes last for the epoch, so a
// composition may reuse those of the one before. Windows (WDS) are not
// needed to place objects and go.
class PgsSubtitleDecoder : public SubtitleDecoder {
 public:
  const char* name() const override { return "pgs"; }
  bool Initialize(const SubtitleConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              std::vector<SubtitleCue>* cues) override;
  void Flush() override;

  // Decodes the run-length coded rows of a |width| x |height| object into
  // palette indices, row by row. Rows the data does not reach stay 0; runs
  // past the end of a row are cut. False if it is not run-length data.
  static bool DecodeRle(const uint8_t* data,
                        size_t size,
                        int width,
                        int height,
                        std::vector<uint8_t>* indices);

  // A palette entry as premultiplied RGBA, packed R first: limited range
  // Y'CbCr in BT.709 above 576 lines of |canvas_height|, else BT.601.
  static uint32_t ToRgba(uint8_t y,
                         uint8_t cr,
                         uint8_t cb,
                         uint8_t alpha,
                         int canvas_height);

 private:
  struct Object {
    int width = 0;
    int height = 0;
    // Run-length data received so far, and how much the object said.
    std::vector<uint8_t> data;
    size_t expected = 0;
  };
  struct Placement {
    int object_id = 0;
    int x = 0;
    int y = 0;
    bool cropped = false;
    int crop_x = 0;
    int crop_y = 0;
    int crop_width = 0;
    int crop_height = 0;
  };
  // Y, Cr, Cb, alpha; alpha 0 for entries no palette set.
  using Palette = std::array<std::array<uint8_t, 4>, 256>;

  void Composition(const uint8_t* data, size_t size, const EncodedPacket& at);
  void PaletteSegment(const uint8_t* data, size_t size);
  void ObjectSegment(const uint8_t* data, size_t size);
  SubtitleCue End();
  void Reset();

  bool composing_ = false;
  int64_t start_ = kNoTimestamp;
  int64_t duration_ = 0;
  int width_ = 0;
  int height_ = 0;
  int palette_id_ = 0;
  std::vector<Placement> placements_;
  std::map<int, Palette> palettes_;
  std::map<int, Object> objects_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_PGS_SUBTITLE_DECODER_H_
