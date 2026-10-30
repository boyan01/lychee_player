#ifndef LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_AUDIO_DECODER_H_
#define LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_AUDIO_DECODER_H_

#include <memory>
#include <vector>

#include "backends/media_foundation/mf_common.h"
#include "media/audio_decoder.h"
#include "media/resampler.h"

namespace lychee {

// AAC (raw with an AudioSpecificConfig, as MP4 stores it, or in ADTS, as
// MPEG-TS does), MP3, FLAC, ALAC, Opus and AC-3 through Media Foundation's
// decoders (FLAC, ALAC and Opus since Windows 10), for builds without
// FFmpeg. Media Foundation's resampler converts to the sink's channels
// and float when they differ, and the library's Resampler to its rate:
// Media Foundation's starts every stream (each loop's pass) a frame late
// and holds back its end.
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
  // sets up |resampler_| and |rate_resampler_|.
  bool SetUpOutput();
  // Collects |mft|'s output until it wants input. False on errors.
  bool Pull(IMFTransform* mft,
            std::vector<Microsoft::WRL::ComPtr<IMFSample>>* samples);
  // Decoded samples through the resamplers (if any) and out; |drain| also
  // empties them.
  bool Deliver(std::vector<Microsoft::WRL::ComPtr<IMFSample>>* decoded,
               bool drain,
               const OutputCallback& output);
  void Emit(IMFSample* sample, const OutputCallback& output);
  // Interleaved float frames at the sink's rate, out.
  void EmitFrames(std::vector<float> samples, const OutputCallback& output);

  AudioConfig config_;
  AudioOutputFormat output_format_;
  Microsoft::WRL::ComPtr<IMFTransform> decoder_;
  Microsoft::WRL::ComPtr<IMFTransform> resampler_;
  // From the decoder's rate to the sink's, if they differ.
  std::unique_ptr<Resampler> rate_resampler_;
  // Whether what Emit() gets is float, else 16-bit integers.
  bool float_output_ = true;
  // For packets without a duration: ALAC's packet length, since its
  // decoder fails samples without one (live input has none).
  int64_t default_duration_ = 0;
  // The decoder's output rate and bytes per frame.
  int decoder_rate_ = 0;
  int decoder_frame_bytes_ = 0;
  // The first output's time, and the frames output since: each output's
  // time counts from it, as adding each one's whole-microsecond duration
  // would fall behind (1024 frames at 48 kHz, 2 frames in 3 s).
  int64_t next_pts_ = kNoTimestamp;
  int64_t output_frames_ = 0;
  // Microseconds of MP3 or Opus fed since the start or a flush, until the
  // first output; -1 after it.
  int64_t expected_us_ = 0;
  int consecutive_errors_ = 0;
  // The FLAC MFT consumes STREAMINFO in-band before its first audio frame.
  bool flac_header_pending_ = true;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MEDIA_FOUNDATION_MF_AUDIO_DECODER_H_
