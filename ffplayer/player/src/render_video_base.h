//
// Created by boyan on 2021/2/16.
//

#ifndef FFP_SRC_RENDER_VIDEO_BASE_H_
#define FFP_SRC_RENDER_VIDEO_BASE_H_

#include <memory>

#include "render_base.h"
#include "ffp_frame_queue.h"
#include "media_clock.h"
#include "ffp_msg_queue.h"
#include "task_runner.h"
#include "demuxer_stream.h"
#include "decoder_video.h"
#include "video_frame.h"
#include "video_renderer_sink.h"

namespace media {

class VideoRenderHost {

 public:

  virtual void OnFirstFrameLoaded(int width, int height) = 0;

  virtual void OnFirstFrameRendered(int width, int height) = 0;

};

class VideoRenderBase : public BaseRender, public std::enable_shared_from_this<VideoRenderBase> {

 public:
  double frame_timer_ = 0;

  bool step = false;

 private:
  int frame_drop_count = 0;
  int frame_drop_count_pre = 0;

  bool abort_render = false;
  bool first_video_frame_loaded_ = false;
  bool first_video_frame_rendered = false;
  int frame_width = 0;
  int frame_height = 0;

  bool force_refresh_ = false;

  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
  double max_frame_duration = 3600;

  bool paused_ = false;

 private:

  // Compute the duration in vp and next_vp.
  double VideoPictureDuration(Frame *vp, Frame *next_vp) const;

  // Compute target frame display delay. For Clock Sync.
  double ComputeTargetDelay(double delay) const;

  // return true if we need drop frames to compensate AV sync.
  bool ShouldDropFrames() const;

 protected:
  int framedrop = -1;

  std::shared_ptr<MediaClock> clock_context;

  virtual void RenderPicture(std::shared_ptr<VideoFrame> frame) = 0;

 public:

  VideoRenderBase();

  ~VideoRenderBase() override;

  void Initialize(VideoRenderHost *host,
                  DemuxerStream *demuxer_stream,
                  std::shared_ptr<MediaClock> clock_ctx,
                  std::shared_ptr<VideoDecoder> decoder
  );

  double GetVideoAspectRatio() const;

  /**
   * called to display each frame
   *
   * @return time for next frame should be scheduled.
   */
  double DrawFrame();

  void Abort() override;

  /// duration is seconds.
  void SetMaxFrameDuration(double duration) { max_frame_duration = duration; }

  bool IsReady() override;

  void Start();

  void Stop();

  /**
   * Flush all frame buffers.
   */
  void Flush();

  void DumpDebugInformation();

 private:

  TaskRunner *task_runner_ = nullptr;

  VideoRenderHost *render_host_ = nullptr;

  std::shared_ptr<VideoDecoder> decoder_;

  CircularDeque<std::shared_ptr<VideoFrame>> frame_queue_;

  void OnNewFrameReady(std::shared_ptr<VideoFrame> frame);

  // To get current clock time in seconds.
  double GetDrawingClock();

  DISALLOW_COPY_AND_ASSIGN(VideoRenderBase);

};

}
#endif //FFP_SRC_RENDER_VIDEO_BASE_H_
