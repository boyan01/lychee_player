#ifndef LYCHEE_BACKENDS_MP4_MP4_SUBTITLE_H_
#define LYCHEE_BACKENDS_MP4_MP4_SUBTITLE_H_

#include <functional>
#include <string_view>
#include <vector>

#include "media/subtitle_decoder.h"

namespace lychee {

// ISO/IEC 14496-30 samples: boxed WebVTT, or IMSC/TTML text and PNG images.
// TTML's internal times belong to the track, before HLS's discontinuity offset.
// Styling is dropped, like the other text subtitle formats. No XML DTD,
// external entities are read. PNG resources are resolved by the demuxer.
using SubtitleResourceReader =
    std::function<bool(std::string_view, std::vector<uint8_t>*)>;
bool ParseMp4Subtitle(std::string_view format,
                      std::string_view sample,
                      int64_t start,
                      int64_t duration,
                      std::vector<SubtitleCue>* cues,
                      const SubtitleResourceReader& resources = {});

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP4_MP4_SUBTITLE_H_
