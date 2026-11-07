#ifndef LYCHEE_BACKENDS_AUDIO_MP3_DEMUXER_H_
#define LYCHEE_BACKENDS_AUDIO_MP3_DEMUXER_H_
#include <array>
#include "backends/audio/audio_demuxer.h"
#include "backends/mp2t/elementary_stream.h"
namespace lychee {
// Layer III frames with optional ID3v2 and Xing/Info or VBRI seek data.
class Mp3Demuxer : public AudioFileDemuxer {
 public:
  const char* name() const override { return "mp3"; }
  static bool Detect(const uint8_t* data, size_t size);
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;

 protected:
  bool Parse() override;

 private:
  bool Find(int64_t* offset, AdtsHeader* header);
  void ReadArtwork(const uint8_t* tag, int64_t end);
  int64_t first_ = 0;
  int64_t audio_start_ = 0;
  int64_t position_ = 0;
  int64_t samples_ = 0;
  int64_t total_samples_ = 0;
  int64_t audio_bytes_ = 0;
  int rate_ = 0;
  bool has_toc_ = false;
  std::array<uint8_t, 100> toc_{};
  struct Point {
    int64_t sample;
    int64_t offset;
  };
  std::vector<Point> vbri_;
};
}  // namespace lychee
#endif
