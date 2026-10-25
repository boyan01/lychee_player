#include "pipeline/live_timeline.h"

#include <algorithm>

#include "base/time.h"

namespace lychee {

namespace {

constexpr int64_t kVideoWindow = 2 * kMicrosPerSecond;
constexpr int64_t kVideoMaxStep = 50 * kMicrosPerMilli;
constexpr int64_t kVideoTolerance = 5 * kMicrosPerMilli;
constexpr int64_t kAudioWindow = kMicrosPerSecond;
// Lead beyond the device's, so a buffer is not cut by scheduling jitter.
constexpr int64_t kAudioMargin = 15 * kMicrosPerMilli;
constexpr int64_t kAudioSlack = 10 * kMicrosPerMilli;
constexpr int64_t kAudioHysteresis = 20 * kMicrosPerMilli;
constexpr int64_t kAudioMaxBehind = 500 * kMicrosPerMilli;
constexpr int kMinWindowPackets = 10;

}  // namespace

bool LiveTimeline::Window::Add(int64_t value, int64_t now, int64_t length) {
  if (count++ == 0) {
    start = now;
    min = value;
  } else {
    min = std::min(min, value);
  }
  return count >= kMinWindowPackets && now - start >= length;
}

int64_t LiveTimeline::Video(int64_t pts, int64_t now) {
  AnchorAt(pts, now);
  // Senders repeat an old timestamp after an idle screen; it says nothing
  // about transit.
  if (pts <= last_video_pts_) {
    return pts + offset_;
  }
  last_video_pts_ = pts;
  if (!video_window_.Add(pts + offset_ - now, now, kVideoWindow)) {
    return pts + offset_;
  }
  const int64_t error = video_window_.min - latency_;
  if (error > kVideoTolerance || error < -kVideoTolerance) {
    offset_ -= std::clamp(error / 2, -kVideoMaxStep, kVideoMaxStep);
  }
  video_window_ = Window();
  return pts + offset_;
}

int64_t LiveTimeline::Audio(int64_t pts, int64_t output_lead, int64_t now) {
  AnchorAt(pts, now);
  if (!audio_anchored_) {
    audio_offset_ = offset_;
    audio_anchored_ = true;
  }
  const int64_t need = std::max<int64_t>(output_lead, 0) + kAudioMargin;
  const int64_t lead = pts + audio_offset_ - now;
  if (lead < need) {
    // Late audio is dropped by the renderer; start later now rather than
    // lose it.
    const int64_t target = std::min(audio_offset_ + need - lead + kAudioSlack,
                                    offset_ + kAudioMaxBehind);
    if (target > audio_offset_) {
      MoveAudio(target);
    }
  } else if (audio_window_.Add(lead - need, now, kAudioWindow)) {
    // Back toward video once the device's lead allows it.
    const int64_t target =
        std::clamp(audio_offset_ - audio_window_.min + kAudioSlack, offset_,
                   offset_ + kAudioMaxBehind);
    if (target > audio_offset_ + kAudioHysteresis ||
        target < audio_offset_ - kAudioHysteresis) {
      MoveAudio(target);
    }
    audio_window_ = Window();
  }
  return pts + audio_offset_;
}

void LiveTimeline::Reset() {
  anchored_ = audio_anchored_ = false;
  last_video_pts_ = INT64_MIN;
  video_window_ = Window();
  audio_window_ = Window();
}

void LiveTimeline::AnchorAt(int64_t pts, int64_t now) {
  if (!anchored_) {
    offset_ = now + latency_ - pts;
    anchored_ = true;
  }
}

void LiveTimeline::MoveAudio(int64_t offset) {
  audio_offset_ = offset;
  audio_window_ = Window();
}

}  // namespace lychee
