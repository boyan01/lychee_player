#ifndef LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_
#define LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_

#include <vector>

#include "backends/legacy/indexed_demuxer.h"

namespace lychee {

// Adobe FLV 10: AAC audio; AVC video, HEVC as codec id 12 (what Chinese
// live services send), and Enhanced RTMP's FourCC video headers (avc1,
// hvc1, av01; veovera/enhanced-rtmp).
class FlvDemuxer final : public IndexedDemuxer {
 public:
  const char* name() const override { return "flv"; }
  static bool Detect(const uint8_t* data, size_t size) {
    return size >= 9 && memcmp(data, "FLV", 3) == 0 && data[3] == 1;
  }

 private:
  // What a tag's payload holds, from its first bytes.
  struct Payload {
    enum Kind { kSkip, kConfig, kFrame, kMetadata } kind = kSkip;
    MediaType type = MediaType::kVideo;
    VideoCodec codec = VideoCodec::kUnknown;
    // Bytes before the configuration or the frame's data.
    size_t skip = 0;
    int64_t cts = 0;
    bool key = true;
  };

  bool Parse() override;
  bool ParseIndexed(int64_t offset);

  // False with |error_| set if the tag is malformed or of a codec this
  // reader lacks.
  bool Inspect(uint8_t type, const uint8_t* head, uint32_t size, Payload* out);
  // A sequence header (configuration record) of |payload|'s stream.
  bool AddConfig(const Payload& payload, std::vector<uint8_t> config);
  void ParseMetadata(const uint8_t* data, size_t size);
  // Fills in what the configurations lack (AV1's picture size).
  void FinishStreams();

  int video_ = -1;
  int audio_ = -1;
  // From onMetaData; 0 if absent.
  double metadata_duration_ = 0;
  int metadata_width_ = 0;
  int metadata_height_ = 0;
  // Per stream: the time between frames.
  std::vector<int64_t> step_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_
