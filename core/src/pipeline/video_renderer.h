#ifndef LYCHEE_PIPELINE_VIDEO_RENDERER_H_
#define LYCHEE_PIPELINE_VIDEO_RENDERER_H_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/frame_converter.h"
#include "media/video_decoder.h"
#include "pipeline/media_clock.h"
#include "pipeline/packet_queue.h"
#include "pipeline/video_renderer_algorithm.h"
#include "pipeline/vsync_estimator.h"

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
  // The ones after it stay as fallbacks for a decoder that fails before its
  // first frame (a hardware decoder rejecting a profile it opened for).
  // |output| is the retained lychee_player_set_video_output() object.
  bool Initialize(const VideoConfig& config,
                  bool allow_hardware,
                  const std::shared_ptr<void>& output,
                  PacketQueue* queue,
                  std::string* error);
  // Passes a new video output to the decoder.
  void SetVideoOutput(std::shared_ptr<void> output);
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
  // A decoded frame lasts past |media_time|, the queue is full, or the
  // stream ended. For SettledForTesting().
  bool HasFramesUntil(int64_t media_time) const;
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
  // Moving average of the time Decode() takes per packet.
  int64_t average_decode_time() const;

 private:
  // Makes the next candidate that accepts |config_| the decoder.
  bool InitializeNextDecoder(std::string* error);
  void DecodeLoop();
  // Decodes |packet|, falling back to the next decoder if this one fails
  // before its first frame. |waited| accumulates time spent in OnFrame().
  bool Decode(const std::shared_ptr<EncodedPacket>& packet,
              uint64_t generation,
              int64_t* waited);
  void OnFrame(std::shared_ptr<VideoFrame> frame, uint64_t generation);
  std::shared_ptr<VideoFrame> Adapt(const std::shared_ptr<VideoFrame>& frame);
  // The frame to show at |display_time|. Runs the algorithm once per vsync
  // interval; later calls within the same interval get the same answer.
  std::shared_ptr<VideoFrame> SelectFrameLocked(int64_t display_time);

  MediaClock* const clock_;
  const uint32_t accepted_storage_;
  PacketQueue* queue_ = nullptr;
  VideoConfig config_;
  // Replaced only on the decode thread (or before it starts), under
  // |mutex_|; other threads read it under |mutex_|.
  std::unique_ptr<VideoDecoder> decoder_;
  // Decode thread only: the decoders still to try, whether |decoder_| has
  // produced a frame, and until it has, the packets it was given since the
  // last flush, to replay into the next one (as Chromium's DecoderStream).
  std::vector<std::unique_ptr<VideoDecoder>> candidates_;
  bool decoder_has_output_ = false;
  std::vector<std::shared_ptr<EncodedPacket>> unconfirmed_packets_;
  std::unique_ptr<FrameConverter> converter_;
  std::thread thread_;
  bool logged_unsupported_frame_ = false;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  VideoRendererAlgorithm algorithm_;
  // The algorithm's choice for the vsync at |selected_vsync_|.
  std::shared_ptr<VideoFrame> selected_;
  int64_t selected_vsync_ = kNoTimestamp;
  VsyncEstimator vsync_estimator_;
  // The frame last handed out by Acquire().
  std::shared_ptr<VideoFrame> current_;
  // |current_| predates the last seek; replace it as soon as possible.
  bool current_stale_ = true;
  // No frame has been selected since the last flush.
  bool first_frame_pending_ = true;
  uint64_t generation_ = 0;
  int64_t start_time_ = kNoTimestamp;
  bool end_of_stream_ = false;
  bool failed_ = false;
  bool stopped_ = false;
  int shown_width_ = 0;
  int shown_height_ = 0;
  int64_t frames_rendered_ = 0;
  int64_t frames_dropped_ = 0;
  int64_t average_decode_time_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_VIDEO_RENDERER_H_
