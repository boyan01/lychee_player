#ifndef LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_
#define LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_
#include "backends/legacy/indexed_demuxer.h"
namespace lychee {
// Adobe FLV 10: AVC and AAC sequence headers, signed composition offsets.
class FlvDemuxer final : public IndexedDemuxer {
 public:
  const char* name() const override { return "flv"; }
  static bool Detect(const uint8_t* data, size_t size) {
    return size >= 9 && memcmp(data, "FLV", 3) == 0 && data[3] == 1;
  }

 private:
  bool Parse() override;
};
}  // namespace lychee
#endif
