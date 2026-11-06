#ifndef LYCHEE_BACKENDS_AUDIO_ADTS_DEMUXER_H_
#define LYCHEE_BACKENDS_AUDIO_ADTS_DEMUXER_H_
#include "backends/audio/audio_demuxer.h"
#include "backends/mp2t/elementary_stream.h"
namespace lychee {
class AdtsDemuxer : public AudioFileDemuxer {
 public:
  const char* name() const override { return "adts"; }
  static bool Detect(const uint8_t* data, size_t size) {
    AdtsHeader h;
    return ParseAdtsHeader(data, size, &h);
  }
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;

 protected:
  bool Parse() override;

 private:
  struct Point {
    int64_t offset;
    int64_t samples;
  };
  std::vector<Point> points_;
  int64_t position_ = 0;
  int64_t samples_ = 0;
  int64_t indexed_end_ = 0;
  int64_t indexed_samples_ = 0;
  int rate_ = 0;
  bool Header(int64_t at, AdtsHeader* h);
  bool Index(int64_t until);
};
}  // namespace lychee
#endif
