#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_SUBTITLE_DECODER_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_SUBTITLE_DECODER_H_

#include "backends/ffmpeg/ffmpeg_common.h"
#include "media/subtitle_decoder.h"

namespace lychee {

// Subtitles through FFmpeg. It turns every text format (SRT, WebVTT,
// mov_text, ASS/SSA, ...) into ASS events, which are reduced to plain text,
// and bitmap formats (PGS, DVB, VobSub) into paletted pictures, which
// become RGBA.
class FFmpegSubtitleDecoder : public SubtitleDecoder {
 public:
  FFmpegSubtitleDecoder();
  ~FFmpegSubtitleDecoder() override;

  const char* name() const override { return "ffmpeg"; }
  bool Initialize(const SubtitleConfig& config, std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              std::vector<SubtitleCue>* cues) override;
  void Flush() override;

 private:
  static SubtitleBitmap ToBitmap(const AVSubtitleRect& rect);

  ScopedAVCodecContext context_;
  ScopedAVPacket av_packet_;
  int consecutive_errors_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_SUBTITLE_DECODER_H_
