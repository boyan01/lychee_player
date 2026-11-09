#ifndef LYCHEE_PIPELINE_AUDIO_GAIN_H_
#define LYCHEE_PIPELINE_AUDIO_GAIN_H_

#include <cstdint>

namespace lychee {

// A level that moves in a straight line to a target over a number of
// frames, one step per frame, so a change does not click.
class LevelRamp {
 public:
  explicit LevelRamp(float level = 1.0f) : current_(level), target_(level) {}

  // Moves to |target| over |frames| frames; at once if |frames| <= 0.
  void Set(float target, int64_t frames);
  // The level for the next frame; advances the ramp by a frame.
  float Next() {
    if (remaining_ == 0) {
      return current_;
    }
    --remaining_;
    // From the target back, so the steps do not add up rounding errors.
    current_ = remaining_ == 0
                   ? target_
                   : target_ - step_ * static_cast<float>(remaining_);
    return current_;
  }
  // Advances by |frames| frames without asking for their levels.
  void Skip(int64_t frames);

  float current() const { return current_; }
  float target() const { return target_; }

 private:
  float current_;
  float target_;
  float step_ = 0;
  int64_t remaining_ = 0;
};

// The level of each output frame: the volume (with its fades) times a
// gain. A gain above 1 (a boost) is lowered to what the loudest sample
// allows, so the output stays within full scale: the peak the caller
// knows (ReplayGain's), or the largest the decoder has produced so far.
// The decoder runs ahead of the output, so the boost comes down before
// such a sample plays. A gain at or below 1 is never changed.
class AudioGain {
 public:
  // Moves the volume (0 to 1) to |volume| over |frames| frames.
  void SetVolume(float volume, int64_t frames);
  // |gain| is linear. |peak|: the largest sample magnitude of the input,
  // or 0 if not known. A change ramps over |frames| frames.
  void SetGain(float gain, float peak, int64_t frames);
  // Decoded audio reached |peak|; a boost comes down over |frames| frames
  // if it would clip that.
  void NotePeak(float peak, int64_t frames);

  // The level for the next frame.
  float Next() { return volume_.Next() * gain_.Next(); }
  void Skip(int64_t frames) {
    volume_.Skip(frames);
    gain_.Skip(frames);
  }
  float volume() const { return volume_.target(); }
  // The gain applied once ramps settle, after the peak limit.
  float gain() const { return gain_.target(); }

 private:
  void Update(int64_t frames);

  LevelRamp volume_;
  LevelRamp gain_;
  float wanted_gain_ = 1.0f;
  float given_peak_ = 0;
  float decoded_peak_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_AUDIO_GAIN_H_
