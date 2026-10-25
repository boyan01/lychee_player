#ifndef LYCHEE_MEDIA_ANNEXB_H_
#define LYCHEE_MEDIA_ANNEXB_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/stream_info.h"

namespace lychee {

// Turns H.264 and HEVC samples as containers store them (length-prefixed
// NAL units, parameter sets in avcC / hvcC; ISO/IEC 14496-15) into the
// Annex B byte stream platform decoders take (MediaCodec, Media
// Foundation). Samples that already are Annex B (MPEG-TS) pass through.
class AnnexBConverter {
 public:
  // False if |config| is not H.264 or HEVC, or its extradata is malformed.
  bool Initialize(const VideoConfig& config);

  // Appends |data| as Annex B to |out|, preceded by the parameter sets if
  // |keyframe|: MP4 keeps them out of band, and a decoder needs them again
  // after every flush. False if a NAL length runs past the end.
  bool Convert(const uint8_t* data,
               size_t size,
               bool keyframe,
               std::vector<uint8_t>* out) const;

  // The parameter sets from the extradata, each NAL unit without a start
  // code: VPS, SPS, PPS (HEVC) or SPS, PPS (H.264), in that order.
  const std::vector<std::vector<uint8_t>>& parameter_sets() const {
    return parameter_sets_;
  }
  // NAL unit type of |nal| (first byte(s) of a NAL unit).
  int NalType(const std::vector<uint8_t>& nal) const;
  bool hevc() const { return hevc_; }

  // Appends |nal| to |out| behind a four-byte start code.
  static void AppendNal(const uint8_t* nal, size_t size,
                        std::vector<uint8_t>* out);

 private:
  bool ParseAvcC(const std::vector<uint8_t>& data);
  bool ParseHvcC(const std::vector<uint8_t>& data);
  bool ParseAnnexB(const std::vector<uint8_t>& data);

  bool hevc_ = false;
  // Bytes of the NAL length prefix; 0 if samples are Annex B already.
  int length_size_ = 4;
  std::vector<std::vector<uint8_t>> parameter_sets_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_ANNEXB_H_
