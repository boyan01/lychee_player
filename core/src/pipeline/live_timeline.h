#ifndef LYCHEE_PIPELINE_LIVE_TIMELINE_H_
#define LYCHEE_PIPELINE_LIVE_TIMELINE_H_

#include <cstdint>

namespace lychee {

// Maps the sender's timestamps of a live input to deadlines in the
// NowMicros() timebase, one session clock for audio and video. The first
// packet only seeds the offset: its transit can be far slower than steady
// state. Video then keeps the earliest arrival of each window |latency|
// ahead of its deadline. Audio follows video unless the output device needs
// more lead; it then plays late by the shortfall instead of dropping
// everything. From flutter-airplay's native/playback/timeline.h.
//
// Not thread-safe.
class LiveTimeline {
 public:
  explicit LiveTimeline(int64_t latency) : latency_(latency) {}

  // The deadline of a video packet stamped |pts| that arrives at |now|.
  int64_t Video(int64_t pts, int64_t now);
  // The deadline of an audio packet stamped |pts| that arrives at |now|.
  // |output_lead| is how far ahead of now the device consumes audio.
  int64_t Audio(int64_t pts, int64_t output_lead, int64_t now);
  // Anchors again on the next packet.
  void Reset();

  int64_t latency() const { return latency_; }
  // How much later than video audio plays, to give the device its lead.
  int64_t audio_behind() const {
    return audio_anchored_ ? audio_offset_ - offset_ : 0;
  }

 private:
  // The minimum over a window of at least kMinWindowPackets values;
  // Add() returns true when the window closes.
  struct Window {
    int64_t start = 0;
    int64_t min = 0;
    int count = 0;
    bool Add(int64_t value, int64_t now, int64_t length);
  };

  void AnchorAt(int64_t pts, int64_t now);
  void MoveAudio(int64_t offset);

  const int64_t latency_;
  bool anchored_ = false;
  bool audio_anchored_ = false;
  int64_t offset_ = 0;
  int64_t audio_offset_ = 0;
  int64_t last_video_pts_ = INT64_MIN;
  Window video_window_;
  Window audio_window_;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_LIVE_TIMELINE_H_
