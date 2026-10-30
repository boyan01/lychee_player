#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_AUDIO_DECODER_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_AUDIO_DECODER_H_

#include "backends/ffmpeg/ffmpeg_common.h"
#include "media/audio_decoder.h"

struct SwrContext;

namespace lychee {

class FFmpegAudioDecoder : public AudioDecoder {
 public:
  FFmpegAudioDecoder();
  ~FFmpegAudioDecoder() override;

  const char* name() const override { return "ffmpeg"; }
  bool Initialize(const AudioConfig& config,
                  const AudioOutputFormat& output_format,
                  std::string* error) override;
  bool Decode(const EncodedPacket& packet,
              const OutputCallback& output) override;
  void Flush() override;

 private:
  void ReceiveFrames(const OutputCallback& output);
  // At the end of the input: what the resampler still holds.
  void DrainResampler(const OutputCallback& output);
  std::shared_ptr<AudioBuffer> Resample(AVFrame* frame);
  bool EnsureResampler(const AVFrame* frame);

  AudioOutputFormat output_format_;
  ScopedAVCodecContext context_;
  ScopedAVFrame frame_;
  ScopedAVPacket av_packet_;
  SwrContext* swr_context_ = nullptr;
  // Input format the resampler was configured for.
  AVChannelLayout swr_layout_ = {};
  int swr_format_ = -1;
  int swr_rate_ = 0;
  // Timestamp of the next output sample, for frames without one.
  int64_t next_pts_ = kNoTimestamp;
  int consecutive_errors_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_AUDIO_DECODER_H_
