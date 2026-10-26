#ifndef LYCHEE_BACKENDS_MP4_MOV_TEXT_DECODER_H_
#define LYCHEE_BACKENDS_MP4_MOV_TEXT_DECODER_H_

#include "media/subtitle_decoder.h"

namespace lychee {

// 3GPP timed text (tx3g, "mov_text"; 3GPP TS 26.245) as plain text, for
// builds without FFmpeg: each sample is a 16-bit length and the text
// (UTF-8, or UTF-16 after a byte order mark), then style boxes, which are
// dropped. An empty sample clears the screen.
class MovTextDecoder : public SubtitleDecoder {
 public:
  const char* name() const override { return "mov_text"; }
  bool Initialize(const SubtitleConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              std::vector<SubtitleCue>* cues) override;
  void Flush() override {}

  // The text of one sample, as Decode() gives it.
  static std::string SampleText(const uint8_t* data, size_t size);
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP4_MOV_TEXT_DECODER_H_
