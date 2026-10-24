#ifndef LYCHEE_PIPELINE_VIDEO_RENDERER_H_
#define LYCHEE_PIPELINE_VIDEO_RENDERER_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "media/frame_converter.h"
#include "media/video_decoder.h"
#include "pipeline/media_clock.h"
#include "pipeline/packet_queue.h"
#include "pipeline/video_renderer_algorithm.h"

namespace lychee {

// Decodes video on its own thread and hands out the frame that should be on
// screen for a given display time. The caller drives presentation (e.g. from
// the UI toolkit's vsync), so frames are pulled, not pushed. Frames are
// chosen by VideoRendererAlgorithm, once per vsync.
class VideoRenderer {
 public:
  VideoRenderer(MediaClock* clock, uint32_t accepted_storage);
  ~VideoRenderer();

  // Tries the decoders in order and keeps the first that accepts |config|.
  bool Initialize(const VideoConfig& config,
                  bool allow_hardware,
                  PacketQueue* queue,
                  std::string* error);
  void Start();
  // The packet queue must be aborted first so the decode thread can exit.
  void Stop();

  // After a seek: drops queued frames, and frames before |start_time| that
  // are still to be decoded.
  void Flush(uint64_t generation, int64_t start_time);

  // A frame is ready to show, or there is nothing left to decode.
  bool HasEnoughData() const;
  // Nothing queued while more frames are expected.
  bool IsStarved() const;
  // The last frame has been shown for its full duration.
  bool Ended(int64_t media_time) const;
  // The decoder failed for good; decoding has stopped.
  bool HasError() const;
  // Coded size of the frame last handed out by Acquire(); false before the
  // first. Streams can change resolution midway (adaptive streaming).
  bool shown_size(int* width, int* height) const;

  bool HasNewFrame(int64_t display_time);
  std::shared_ptr<VideoFrame> Acquire(int64_t display_time);

  const char* decoder_name() const;
  bool is_hardware() const;
  int64_t frames_rendered() const;
  int64_t frames_dropped() const;

 private:
  void DecodeLoop();
  void OnFrame(std::shared_ptr<VideoFrame> frame, uint64_t generation);
  std::shared_ptr<VideoFrame> Adapt(const std::shared_ptr<VideoFrame>& frame);
  // The frame to show at |display_time|. Runs the algorithm once per vsync
  // interval; later calls within the same interval get the same answer.
  std::shared_ptr<VideoFrame> SelectFrameLocked(int64_t display_time);
  // Vsync interval, estimated from the display times the caller passes:
  // the API has no refresh rate.
  int64_t RenderIntervalLocked(int64_t display_time);
  static int64_t EstimateRenderInterval(
      const std::deque<int64_t>& display_intervals);
  // Moves |display_time| onto the estimated vsync grid. Callers that only
  // know when they woke up pass jittered times; the algorithm needs regular
  // intervals.
  int64_t SnapToVsyncLocked(int64_t display_time, int64_t interval);

  MediaClock* const clock_;
  const uint32_t accepted_storage_;
  PacketQueue* queue_ = nullptr;
  std::unique_ptr<VideoDecoder> decoder_;
  std::unique_ptr<FrameConverter> converter_;
  std::thread thread_;
  bool logged_unsupported_frame_ = false;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  VideoRendererAlgorithm algorithm_;
  // The algorithm's choice for the vsync at |selected_vsync_|.
  std::shared_ptr<VideoFrame> selected_;
  int64_t selected_vsync_ = kNoTimestamp;
  int64_t vsync_grid_ = kNoTimestamp;
  int64_t last_display_time_ = kNoTimestamp;
  std::deque<int64_t> display_intervals_;
  int64_t render_interval_ = 0;
  int unexpected_intervals_ = 0;
  // The frame last handed out by Acquire().
  std::shared_ptr<VideoFrame> current_;
  // |current_| predates the last seek; replace it as soon as possible.
  bool current_stale_ = true;
  uint64_t generation_ = 0;
  int64_t start_time_ = kNoTimestamp;
  bool end_of_stream_ = false;
  bool failed_ = false;
  bool stopped_ = false;
  int shown_width_ = 0;
  int shown_height_ = 0;
  int64_t frames_rendered_ = 0;
  int64_t frames_dropped_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_VIDEO_RENDERER_H_
