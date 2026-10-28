#ifndef LYCHEE_BACKENDS_MP2T_ELEMENTARY_STREAM_H_
#define LYCHEE_BACKENDS_MP2T_ELEMENTARY_STREAM_H_

#include <cstddef>
#include <cstdint>

#include "media/stream_info.h"

namespace lychee {

// What an Annex B access unit (one PES packet of H.264 or HEVC) says
// about itself, from the NAL units before its first slice.
struct AccessUnitInfo {
  // An IDR picture (H.264) or an IRAP picture (HEVC).
  bool keyframe = false;
  // Its SPS, if it carries one, filled into width, height, sample aspect
  // ratio and (H.264) colour of a VideoConfig.
  bool has_sps = false;
  int width = 0;
  int height = 0;
  int sar_num = 1;
  int sar_den = 1;
  ColorInfo color;
};

// Scans |data| (Annex B) up to its first slice. False if it holds no
// NAL unit at all.
bool ParseAccessUnit(const uint8_t* data,
                     size_t size,
                     bool hevc,
                     AccessUnitInfo* info);

// One ADTS header (ISO/IEC 13818-7 / 14496-3).
struct AdtsHeader {
  int object_type = 0;  // AAC LC is 2.
  int sample_rate = 0;
  int channels = 0;
  // The whole frame, header included.
  size_t frame_length = 0;
  size_t header_size = 0;
  // PCM frames it decodes to.
  int samples = 0;
};

// The header at |data|, if it is a plausible one that fits in |size|
// bytes of header (the frame may extend past |size|).
bool ParseAdtsHeader(const uint8_t* data, size_t size, AdtsHeader* header);

// One MPEG audio Layer III frame header (ISO/IEC 11172-3, 13818-3, and
// MPEG 2.5), as an AdtsHeader: header_size 4, object_type 0. False for
// other layers, a free bitrate or reserved values.
bool ParseMp3Header(const uint8_t* data, size_t size, AdtsHeader* header);

// The sample rate and channels (LFE included) of an AC-3 or E-AC-3
// syncframe (ATSC A/52) that |data| starts with, into |header|; the
// other fields are left alone. False if it does not start with one.
bool ParseAc3Header(const uint8_t* data, size_t size, AdtsHeader* header);

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP2T_ELEMENTARY_STREAM_H_
