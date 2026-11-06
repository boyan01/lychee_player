#ifndef LYCHEE_BACKENDS_AUDIO_WAV_DEMUXER_H_
#define LYCHEE_BACKENDS_AUDIO_WAV_DEMUXER_H_
#include "backends/audio/audio_demuxer.h"
namespace lychee {
class WavDemuxer : public AudioFileDemuxer {
 public:
  const char* name() const override { return "wav"; }
  static bool Detect(const uint8_t* p, size_t n) {
    return n >= 12 && !memcmp(p, "RIFF", 4) && !memcmp(p + 8, "WAVE", 4);
  }
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;

 protected:
  bool Parse() override;

 private:
  int64_t start_ = 0;
  int64_t end_ = 0;
  int64_t position_ = 0;
  int block_ = 0;
  int rate_ = 0;
};
}  // namespace lychee
#endif
