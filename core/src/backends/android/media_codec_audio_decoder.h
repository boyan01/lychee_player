#ifndef LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_AUDIO_DECODER_H_
#define LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_AUDIO_DECODER_H_

#include <media/NdkMediaCodec.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/audio_decoder.h"
#include "media/resampler.h"

namespace lychee {

// AAC (including ADTS), MP3, Opus, FLAC and ALAC (Android 13 and later)
// through NDK MediaCodec, for builds without FFmpeg. MediaCodec only
// decodes: the channels are mixed to the sink's (ChannelMatrix) and the
// rate converted with Resampler. Output times count frames from the
// codec's first output after a flush.
class MediaCodecAudioDecoder : public AudioDecoder {
 public:
  MediaCodecAudioDecoder() = default;
  ~MediaCodecAudioDecoder() override;

  const char* name() const override { return "mediacodec"; }
  bool Initialize(const AudioConfig& config,
                  const AudioOutputFormat& output_format,
                  std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;
  void Restart() override;

 private:
  bool CreateCodec(std::string* error);
  void DeleteCodec();
  // Waits for an input buffer, handing out decoded audio meanwhile.
  bool QueueInput(const uint8_t* data,
                  size_t size,
                  int64_t pts,
                  uint32_t flags,
                  const OutputCallback& output);
  // Hands out all decoded audio available. False on a codec error.
  bool DrainOutput(const OutputCallback& output);
  // Takes the codec's output rate, channels and sample format.
  void ReadOutputFormat();
  // Hands out |resampled_| as one buffer.
  void Emit(const OutputCallback& output);
  // Bytes of the ADTS header |packet| starts with, or 0.
  size_t AdtsHeaderSize(const EncodedPacket& packet) const;

  AudioConfig config_;
  AudioOutputFormat output_format_;
  const char* mime_ = nullptr;
  // csd-0: the codec configuration in MediaCodec's format.
  std::vector<uint8_t> csd_;
  AMediaCodec* codec_ = nullptr;
  std::string codec_name_;
  // Since the codec started or was flushed: MediaCodec may lose its
  // configuration in a flush before any output.
  bool had_output_ = false;
  // What the codec outputs.
  int rate_ = 0;
  int channels_ = 0;
  bool float_output_ = false;
  std::vector<float> matrix_;
  std::unique_ptr<Resampler> resampler_;
  std::vector<float> decoded_;
  std::vector<float> mixed_;
  std::vector<float> resampled_;
  // The first output's time since the last flush, and the frames handed
  // out since.
  int64_t anchor_pts_ = kNoTimestamp;
  int64_t frames_out_ = 0;
  AudioTimestampFollower timestamps_;
  int64_t last_pts_ = 0;
  bool end_of_stream_ = false;
  // Opus distinguishes a source restart from a midstream seek by a zero
  // input timestamp. Loop packets have offsets on the player's timeline.
  bool opus_at_start_ = true;
  int64_t opus_input_offset_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_ANDROID_MEDIA_CODEC_AUDIO_DECODER_H_
