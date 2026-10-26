#ifndef LYCHEE_MEDIA_RESAMPLER_H_
#define LYCHEE_MEDIA_RESAMPLER_H_

#include <cstdint>
#include <vector>

namespace lychee {

// Converts interleaved float audio from one sample rate to another with a
// windowed sinc filter, for decoders whose platform has no resampler
// (MediaCodec). Output frame n is the input at time n * input_rate /
// output_rate: the filter is centered, so it adds no delay to timestamps,
// only holds back the last few input frames until more come or Drain().
class Resampler {
 public:
  Resampler(int channels, int input_rate, int output_rate);

  // Appends the output |frames| more input frames make available.
  void Push(const float* input, int frames, std::vector<float>* output);
  // Ends the stream: appends the output up to the end of the input, then
  // starts over as if new.
  void Drain(std::vector<float>* output);
  void Reset();

 private:
  // Produces output frames up to input frame |end| of buffer_.
  void Produce(int64_t end, std::vector<float>* output);

  const int channels_;
  const int64_t input_rate_;
  const int64_t output_rate_;
  // Kernels for kPhases + 1 fractional offsets, kTaps each.
  std::vector<float> kernels_;
  // Input frames, interleaved, from kTaps / 2 - 1 frames before time_.
  std::vector<float> buffer_;
  // Where the next output frame falls, in frames of buffer_ times
  // output_rate_, so that steps of input_rate_ add up exactly.
  int64_t time_ = 0;
};

// The other half of what MediaCodec leaves to the caller: weights that mix
// |in| channels, in Android's (WAVE's) order FL FR FC LFE BL BR SL SR, into
// |out|, as matrix[o * in + i]. Fewer channels go to the first outputs
// (mono to both fronts); more are mixed down to stereo without LFE, scaled
// so a full-scale signal in every channel does not clip.
std::vector<float> ChannelMatrix(int in, int out);

}  // namespace lychee

#endif  // LYCHEE_MEDIA_RESAMPLER_H_
