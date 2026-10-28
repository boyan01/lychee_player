#ifndef LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_AUDIO_DECODER_H_
#define LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_AUDIO_DECODER_H_

#include <vector>

#include "backends/media_foundation/mf_common.h"
#include "media/audio_decoder.h"

namespace lychee {

// AAC (raw with an AudioSpecificConfig, as MP4 stores it, or in ADTS, as
// MPEG-TS does), MP3 and FLAC through Media Foundation's decoders (FLAC since
// Windows 10), for builds without FFmpeg. Media Foundation's resampler converts
// to the sink's rate and channels when they differ.
class MFAudioDecoder : public AudioDecoder {
 public:
  MFAudioDecoder() = default;
  ~MFAudioDecoder() override = default;

  const char* name() const override { return "mediafoundation"; }
  bool Initialize(const AudioConfig& config,
                  const AudioOutputFormat& output_format,
                  std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  // Creates |decoder_| for |config_| with |extradata| (for ADTS, an
  // AudioSpecificConfig made from the first header).
  bool Open(const std::vector<uint8_t>& extradata, std::string* error);
  // Picks the decoder's output type and, if it is not the sink's format,
  // sets up |resampler_|.
  bool SetUpOutput();
  // Collects |mft|'s output until it wants input. False on errors.
  bool Pull(IMFTransform* mft,
            std::vector<Microsoft::WRL::ComPtr<IMFSample>>* samples);
  // Decoded samples through the resampler (if any) and out; |drain| also
  // empties the resampler.
  bool Deliver(std::vector<Microsoft::WRL::ComPtr<IMFSample>>* decoded,
               bool drain,
               const OutputCallback& output);
  void Emit(IMFSample* sample, const OutputCallback& output);

  AudioConfig config_;
  AudioOutputFormat output_format_;
  Microsoft::WRL::ComPtr<IMFTransform> decoder_;
  Microsoft::WRL::ComPtr<IMFTransform> resampler_;
  // Whether what Emit() gets is float, else 16-bit integers.
  bool float_output_ = true;
  int64_t next_pts_ = kNoTimestamp;
  int consecutive_errors_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_AUDIO_DECODER_H_
