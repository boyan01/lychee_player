#ifndef LYCHEE_BACKENDS_AUDIO_OGG_DEMUXER_H_
#define LYCHEE_BACKENDS_AUDIO_OGG_DEMUXER_H_
#include <deque>
#include "backends/audio/audio_demuxer.h"
namespace lychee {
class OggDemuxer : public AudioFileDemuxer {
 public:
  const char* name() const override { return "ogg"; }
  static bool Detect(const uint8_t* p, size_t n) {
    return n >= 4 && !memcmp(p, "OggS", 4);
  }
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;

 protected:
  bool Parse() override;

 private:
  struct Page {
    int64_t offset = 0;
    int64_t end = 0;
    int64_t granule = -1;
    uint32_t serial = 0;
    uint32_t sequence = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> laces;
    std::vector<uint8_t> bytes;
  };
  bool PageAt(int64_t at, Page* page);
  bool FindPage(int64_t* at, Page* page, int64_t limit);
  bool Fill();
  bool Header(const std::vector<uint8_t>& bytes);
  int PacketSamples(const std::vector<uint8_t>& bytes);
  std::deque<EncodedPacket> ready_;
  std::vector<uint8_t> partial_;
  std::vector<std::vector<uint8_t>> headers_;
  std::vector<bool> modes_;
  int64_t position_ = 0;
  int64_t audio_start_ = -1;
  int64_t next_sample_ = 0;
  int64_t last_granule_ = 0;
  uint32_t serial_ = 0;
  uint32_t sequence_ = 0;
  bool have_serial_ = false;
  bool have_sequence_ = false;
  bool discard_continued_ = false;
  bool ended_ = false;
  int header_count_ = 0;
  int rate_ = 0;
  int preskip_ = 0;
  int small_block_ = 0;
  int large_block_ = 0;
  int previous_block_ = 0;
};
}  // namespace lychee
#endif
