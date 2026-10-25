#ifndef LYCHEE_PIPELINE_VIDEO_RENDERER_H_
#define LYCHEE_PIPELINE_VIDEO_RENDERER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/frame_converter.h"
#include "media/video_decoder.h"
#include "media/video_device.h"
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
  // |rotate_surface_frames|: see CreateVideoDecoders().
  VideoRenderer(MediaClock* clock,
                uint32_t accepted_storage,
                bool rotate_surface_frames);
  ~VideoRenderer();

  // Tries the decoders in order and keeps the first that accepts |config|.
  // The ones after it stay as fallbacks for a decoder that fails before its
  // first frame (a hardware decoder rejecting a profile it opened for).
  // |surface| is the retained lychee_player_set_video_surface() window.
  bool Initialize(const VideoConfig& config,
                  bool allow_hardware,
                  const VideoDevice& device,
                  const std::shared_ptr<void>& surface,
                  PacketQueue* queue,
                  std::string* error);
  // Passes a new surface to the decoder.
  void SetVideoSurface(std::shared_ptr<void> surface);
  // The device the decoders were created for.
  const VideoDevice& device() const { return device_; }
  void Start();
  // The packet queue must be aborted first so the decode thread can exit.
  void Stop();

  // After a seek: drops queued frames, and frames before |start_time| that
  // are still to be decoded. |continue_shown|: also those up to the end of
  // the frame on screen, for a resync that should not show frames again
  // (callers pull frames for display times ahead of the clock).
  void Flush(uint64_t generation, int64_t start_time, bool continue_shown);
  // Where the frame on screen ends, or kNoTimestamp if none or stale.
  int64_t shown_end() const;
  // A renderer replacing another: what it showed, for Flush().
  void ContinueFrom(int64_t shown_end);

  // A frame is ready to show, or there is nothing left to decode.
  bool HasEnoughData() const;
  // Nothing queued while more frames are expected.
  bool IsStarved() const;
  // The last frame has been shown for its full duration.
  bool Ended(int64_t media_time) const;
  // Playback ends at |time| (kNoTimestamp: at the end of the stream): later
  // frames are neither shown nor decoded ahead. Cleared by Flush().
  void SetEndTime(int64_t time);
  // A decoded frame lasts past |media_time|, the queue is full, or the
  // stream ended. For SettledForTesting().
  bool HasFramesUntil(int64_t media_time) const;
  // The decoder failed for good; decoding has stopped.
  bool HasError() const;
  // A hardware decoder failed after its first frame, which is how a lost
  // device shows (a GPU reset, MediaCodec reclaiming the codec). Decoding
  // has stopped; a new renderer can take over at the current position.
  bool DecoderLost() const;
  // Makes the decoder count as lost at the next packet, for tests.
  void SimulateDecoderLossForTesting() { simulate_loss_ = true; }
  // Coded size of the frame last handed out by Acquire(); false before the
  // first. Streams can change resolution midway (adaptive streaming).
  bool shown_size(int* width, int* height) const;

  // When nobody has pulled a frame for a while (the app is in the
  // background, its view hidden), drops the frames already past |now|
  // (NowMicros()) without counting them as dropped, so decoding keeps up
  // with the clock and the pull resumes at the clock's position. Chromium's
  // VideoFrameCompositor renders in the background the same way, after
  // 250 ms without a vsync (media/renderers/video_renderer_impl.cc does not
  // count those drops). Called periodically.
  void ExpireFramesWhileHidden(int64_t now);

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
  const bool rotate_surface_frames_;
  VideoDevice device_;
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
  // NowMicros() of the last HasNewFrame() or Acquire().
  int64_t last_pull_ = kNoTimestamp;
  // The frame last handed out by Acquire().
  std::shared_ptr<VideoFrame> current_;
  // |current_| predates the last seek; replace it as soon as possible.
  bool current_stale_ = true;
  // No frame has been selected since the last flush.
  bool first_frame_pending_ = true;
  uint64_t generation_ = 0;
  int64_t start_time_ = kNoTimestamp;
  // ContinueFrom(), until the first flush.
  int64_t inherited_shown_end_ = kNoTimestamp;
  int64_t end_time_ = kNoTimestamp;
  bool end_of_stream_ = false;
  bool failed_ = false;
  bool decoder_lost_ = false;
  std::atomic<bool> simulate_loss_{false};
  bool stopped_ = false;
  int shown_width_ = 0;
  int shown_height_ = 0;
  int64_t frames_rendered_ = 0;
  int64_t frames_dropped_ = 0;
  int64_t average_decode_time_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_VIDEO_RENDERER_H_
