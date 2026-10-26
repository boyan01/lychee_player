#ifndef LYCHEE_MEDIA_TEXT_SUBTITLE_DECODER_H_
#define LYCHEE_MEDIA_TEXT_SUBTITLE_DECODER_H_

#include <string>
#include <string_view>
#include <vector>

#include "media/subtitle_decoder.h"

namespace lychee {

// Text subtitles without FFmpeg: SubRip, ASS/SSA, WebVTT and mov_text
// (tx3g) packets as plain text cues, with their markup dropped as FFmpeg's
// decoders and AssToPlainText() drop it. Bitmap subtitles are not decoded.
class TextSubtitleDecoder : public SubtitleDecoder {
 public:
  const char* name() const override { return "text"; }
  bool Initialize(const SubtitleConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              std::vector<SubtitleCue>* cues) override;
  void Flush() override {}

  // The plain text of one packet in |codec_name|'s format.
  static std::string ToPlainText(const std::string& codec_name,
                                 std::string_view packet);

 private:
  std::string codec_name_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_TEXT_SUBTITLE_DECODER_H_
