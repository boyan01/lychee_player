#ifndef LYCHEE_MEDIA_PCM_AUDIO_DECODER_H_
#define LYCHEE_MEDIA_PCM_AUDIO_DECODER_H_
#include "media/audio_decoder.h"
#include "media/resampler.h"
namespace lychee {
// Uncompressed WAVE samples need no platform codec. The existing common
// mixer and resampler produce the same sink format on every platform.
class PcmAudioDecoder : public AudioDecoder {
 public:
  const char* name() const override { return "pcm"; }
  bool Initialize(const AudioConfig& config,
                  const AudioOutputFormat& format,
                  std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  AudioConfig config_;
  AudioOutputFormat format_;
  int bits_ = 0;
  bool float_ = false;
  bool unsigned_ = false;
  int64_t anchor_ = kNoTimestamp;
  int64_t frames_ = 0;
  std::vector<float> matrix_;
  std::unique_ptr<Resampler> resampler_;
};
}  // namespace lychee
#endif
