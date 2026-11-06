#ifndef LYCHEE_BACKENDS_AUDIO_FLAC_DEMUXER_H_
#define LYCHEE_BACKENDS_AUDIO_FLAC_DEMUXER_H_
#include "backends/audio/audio_demuxer.h"
namespace lychee {
class FlacDemuxer : public AudioFileDemuxer {
 public:
  const char* name() const override { return "flac"; }
  static bool Detect(const uint8_t* p, size_t n) {
    return n >= 4 && !memcmp(p, "fLaC", 4);
  }
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;

 protected:
  bool Parse() override;

 private:
  struct Header {
    int64_t sample = 0;
    int block = 0;
  };
  struct Point {
    int64_t sample;
    int64_t offset;
  };
  bool FrameHeader(int64_t at, Header* h);
  bool Find(int64_t* at, Header* h, int64_t limit);
  std::vector<Point> points_;
  int64_t start_ = 0;
  int64_t position_ = 0;
  int64_t total_ = 0;
  int rate_ = 0;
  int block_ = 0;
};
}  // namespace lychee
#endif
