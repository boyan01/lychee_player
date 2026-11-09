#include "pipeline/audio_tap.h"

#include <algorithm>
#include <cmath>

namespace lychee {

namespace {

// Devices report when each buffer plays with some jitter; a buffer this
// close to where the last one ended continues it.
constexpr int64_t kJitter = 2 * kMicrosPerMilli;

}  // namespace

void AudioTap::Initialize(int sample_rate, int capacity) {
  sample_rate_ = sample_rate;
  ring_.assign(static_cast<size_t>(std::max(capacity, 1)), 0.0f);
  begin_ = end_ = 0;
  anchor_time_ = kNoTimestamp;
}

void AudioTap::Write(const float* samples,
                     int channels,
                     int written,
                     int frames,
                     int64_t playout_time) {
  if (ring_.empty() || frames <= 0) {
    return;
  }
  const auto size = static_cast<int64_t>(ring_.size());
  if (anchor_time_ == kNoTimestamp) {
    begin_ = end_;
  } else {
    const int64_t expected =
        anchor_time_ + (end_ - anchor_frame_) * kMicrosPerSecond / sample_rate_;
    const int64_t gap = playout_time - expected;
    if (std::abs(gap) <= kJitter) {
      // Contiguous: keep the anchor, so the timeline does not drift by a
      // rounding per buffer.
      playout_time = kNoTimestamp;
    } else if (gap > 0 && gap * sample_rate_ / kMicrosPerSecond < size) {
      // The device played nothing of ours meanwhile (a pause): silence.
      const int64_t silent = gap * sample_rate_ / kMicrosPerSecond;
      for (int64_t i = 0; i < silent; ++i) {
        ring_[static_cast<size_t>((end_ + i) % size)] = 0.0f;
      }
      end_ += silent;
    } else {
      // Long ago, or overlapping what came before: start over.
      begin_ = end_;
    }
  }
  if (playout_time != kNoTimestamp) {
    anchor_time_ = playout_time;
    anchor_frame_ = end_;
  }
  const float scale = 1.0f / static_cast<float>(channels);
  for (int i = 0; i < frames; ++i) {
    float sum = 0;
    if (i < written) {
      for (int k = 0; k < channels; ++k) {
        sum += samples[i * channels + k];
      }
    }
    ring_[static_cast<size_t>((end_ + i) % size)] = sum * scale;
  }
  end_ += frames;
  begin_ = std::max(begin_, end_ - size);
}

void AudioTap::Read(int64_t time, float* out, int frames) const {
  if (frames <= 0) {
    return;
  }
  if (anchor_time_ == kNoTimestamp) {
    std::fill_n(out, frames, 0.0f);
    return;
  }
  const auto size = static_cast<int64_t>(ring_.size());
  // The frame after the last one heard by |time|.
  const int64_t last =
      anchor_frame_ + static_cast<int64_t>(std::llround(
                          static_cast<double>(time - anchor_time_) *
                          sample_rate_ / kMicrosPerSecond));
  for (int i = 0; i < frames; ++i) {
    const int64_t frame = last - frames + i;
    out[i] = frame >= begin_ && frame < end_
                 ? ring_[static_cast<size_t>(frame % size)]
                 : 0.0f;
  }
}

}  // namespace lychee
