#ifndef LYCHEE_PIPELINE_THUMBNAILER_H_
#define LYCHEE_PIPELINE_THUMBNAILER_H_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/demuxer.h"
#include "media/frame_converter.h"
#include "media/video_decoder.h"
#include "media/video_frame.h"

namespace lychee {

class SurfaceFrameReader;

// Pictures of an input at given positions, apart from any player
// (lychee_thumbnailer): its own demuxer and video decoder, opened by the
// first request and kept for the next. Each request seeks to the keyframe
// at or before the position and decodes from there, the keyframe alone or
// (exact) up to the frame showing at the position. Frames come out as
// upright kCpuRgba scaled to fit the maximum size.
//
// Thread-safe. Get() runs on the caller's thread, one at a time.
class Thumbnailer {
 public:
  // |headers| for the input's http(s) requests (NormalizeHttpHeaders()).
  // Maximum size 0: unlimited.
  Thumbnailer(std::string url,
              std::string headers,
              int max_width,
              int max_height);
  // Interrupts Get() calls in progress and waits for them to return.
  ~Thumbnailer();
  Thumbnailer(const Thumbnailer&) = delete;
  Thumbnailer& operator=(const Thumbnailer&) = delete;

  // Blocking. |position| as lychee_player_seek() takes it. Null if the
  // input could not be opened, has no video, could not seek there or
  // decode, or Cancel() came first.
  std::shared_ptr<VideoFrame> Get(int64_t position, bool exact);

  // Makes the Get() calls in progress, a network read in flight too,
  // return null promptly. Later calls open the input again.
  void Cancel();

 private:
  // Get() once counted in |calls_|; |cancels| is |cancels_| at its start.
  std::shared_ptr<VideoFrame> GetCounted(int64_t position,
                                         bool exact,
                                         uint64_t cancels);
  bool CancelledLocked(uint64_t cancels);
  // With |get_mutex_| held.
  bool OpenLocked(uint64_t cancels);
  void CloseLocked();
  bool InitializeNextDecoderLocked();
  std::shared_ptr<VideoFrame> DecodeLocked(int64_t position, bool exact);
  // The decoder's |frame| as upright kCpuRgba of the thumbnail's size.
  std::shared_ptr<VideoFrame> FinishLocked(
      const std::shared_ptr<VideoFrame>& frame);

  const std::string url_;
  const std::string headers_;
  const int max_width_;
  const int max_height_;

  // Serializes Get(); owns everything below up to |mutex_|.
  std::mutex get_mutex_;
  std::unique_ptr<Demuxer> demuxer_holder_;
  int stream_ = -1;
  VideoConfig config_;
#if defined(__ANDROID__)
  // Outlives the decoders, which render into it.
  std::shared_ptr<SurfaceFrameReader> surface_reader_;
#endif
  std::vector<std::unique_ptr<VideoDecoder>> candidates_;
  std::unique_ptr<VideoDecoder> decoder_;
  bool decoder_has_output_ = false;
  std::unique_ptr<FrameConverter> converter_;
  // Nothing has been read since the open: input that cannot seek still
  // reads from its start.
  bool fresh_ = false;

  std::mutex mutex_;
  std::condition_variable condition_;
  // Destroying.
  bool aborted_ = false;
  // Get() calls in progress, waiting for |get_mutex_| included.
  int calls_ = 0;
  // Cancel() calls, and their count when |demuxer_| opened.
  uint64_t cancels_ = 0;
  uint64_t opened_cancels_ = 0;
  // |demuxer_holder_|, for Cancel() from another thread.
  Demuxer* demuxer_ = nullptr;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_THUMBNAILER_H_
