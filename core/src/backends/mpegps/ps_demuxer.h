#ifndef LYCHEE_BACKENDS_MPEGPS_PS_DEMUXER_H_
#define LYCHEE_BACKENDS_MPEGPS_PS_DEMUXER_H_
#include "backends/legacy/indexed_demuxer.h"
namespace lychee {
// MPEG-1/2 system headers and PES, with complete MPEG-2 video pictures.
class PsDemuxer final : public IndexedDemuxer {
 public:
  const char* name() const override { return "mpegps"; }
  static bool Detect(const uint8_t* data, size_t size) {
    return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 1 &&
           data[3] == 0xba;
  }

 private:
  struct Extent {
    int64_t logical, physical;
    uint32_t size;
  };
  struct Stamp {
    int64_t offset, pts, dts;
  };
  bool Parse() override;
  bool Load(const Sample& sample, std::vector<uint8_t>* bytes) override;
  bool Video(int64_t physical, uint32_t size, int64_t pts, int64_t dts);
  bool Pictures(bool final);
  bool Picture(size_t size);
  bool Audio(int64_t physical, uint32_t size, int64_t pts, bool ac3);
  std::vector<Extent> extents_;
  std::vector<Extent> audio_extents_;
  std::vector<uint8_t> audio_pending_;
  int64_t audio_logical_ = 0, audio_offset_ = 0, audio_pts_ = -1;
  std::vector<Stamp> stamps_;
  std::vector<uint8_t> pending_;
  int64_t logical_ = 0, pending_offset_ = 0;
  int video_ = -1, audio_ = -1;
  int audio_id_ = -1, substream_id_ = -1;
  int64_t frame_us_ = 40000, next_dts_ = 0, gop_pts_ = 0;
  size_t stamp_ = 0;
  std::vector<uint8_t> sequence_;
};
}  // namespace lychee
#endif
