#ifndef LYCHEE_BACKENDS_AVI_AVI_DEMUXER_H_
#define LYCHEE_BACKENDS_AVI_AVI_DEMUXER_H_
#include <map>
#include "backends/legacy/indexed_demuxer.h"
namespace lychee {
class AviDemuxer final : public IndexedDemuxer {
 public:
  const char* name() const override { return "avi"; }
  static bool Detect(const uint8_t* data, size_t size) {
    return size >= 12 && !memcmp(data, "RIFF", 4) &&
           !memcmp(data + 8, "AVI ", 4);
  }

 private:
  struct Track {
    int index = -1;
    uint32_t scale = 0, rate = 0, sample_size = 0;
    int64_t units = 0;
  };
  bool Parse() override;
  bool Chunks(int64_t begin, int64_t end, int depth, int track, bool movie);
  bool Format(int track, int64_t offset, uint32_t size);
  bool Packet(int track, int64_t offset, uint32_t size);
  bool Index(int64_t offset, uint32_t size);
  std::vector<Track> tracks_;
  std::map<int64_t, bool> keyframes_;
  int64_t movi_ = -1;
};
}  // namespace lychee
#endif
