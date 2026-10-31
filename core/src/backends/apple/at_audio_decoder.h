#ifndef LYCHEE_BACKENDS_APPLE_AT_AUDIO_DECODER_H_
#define LYCHEE_BACKENDS_APPLE_AT_AUDIO_DECODER_H_

#include <AudioToolbox/AudioToolbox.h>

#include <vector>

#include "media/audio_decoder.h"

namespace lychee {

// AAC (LC, HE, HE v2), ALAC, Opus, FLAC and MP3 through AudioToolbox,
// without FFmpeg. One AudioConverter decodes and converts to the sink's
// rate and channels.
class ATAudioDecoder : public AudioDecoder {
 public:
  ATAudioDecoder() = default;
  ~ATAudioDecoder() override;

  const char* name() const override { return "audiotoolbox"; }
  bool Initialize(const AudioConfig& config,
                  const AudioOutputFormat& output_format,
                  std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  static OSStatus Input(AudioConverterRef converter,
                        UInt32* packets,
                        AudioBufferList* data,
                        AudioStreamPacketDescription** descriptions,
                        void* decoder);

  bool CreateConverter(const std::vector<uint8_t>& extradata);
  // Bytes of the ADTS header |packet| starts with, or 0.
  size_t AdtsHeaderSize(const EncodedPacket& packet) const;

  AudioConfig config_;
  AudioOutputFormat output_format_;
  AudioConverterRef converter_ = nullptr;
  // The packet Input() hands to the converter next, if any.
  const EncodedPacket* pending_ = nullptr;
  bool draining_ = false;
  AudioStreamPacketDescription description_ = {};
  // The first output's time, and the frames output since: each output's
  // time counts from it, as adding each one's whole-microsecond duration
  // would fall behind (1024 frames at 48 kHz, 2 frames in 3 s).
  int64_t next_pts_ = kNoTimestamp;
  int64_t output_frames_ = 0;
  AudioTimestampFollower timestamps_;
  // The next output is the first since the converter started: Opus and MP3
  // lose the decoder's delay from it.
  bool first_output_ = true;
  int consecutive_errors_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_APPLE_AT_AUDIO_DECODER_H_
