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

class VideoRenderBase : public BaseRender {

 public:
  double frame_timer = 0;

  bool step = false;

 private:
  int frame_drop_count = 0;
  int frame_drop_count_pre = 0;

  bool abort_render = false;
  bool first_video_frame_loaded = false;
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
  std::unique_ptr<FrameQueue> picture_queue;
  int framedrop = -1;

  std::shared_ptr<MediaClock> clock_context;
  std::shared_ptr<MessageContext> msg_ctx_;

  virtual void RenderPicture(Frame &frame) = 0;

 public:

  VideoRenderBase();

  ~VideoRenderBase() override;

  void Init(const std::shared_ptr<PacketQueue> &video_queue, std::shared_ptr<MediaClock> clock_ctx,
            std::shared_ptr<MessageContext> msg_ctx);

  /**
   * Accept Frame from Decoder.
   *
   * @param src_frame source frame.
   * @param pts frame presentation timestamp in seconds.
   * @param duration frame presentation duration in seconds.
   * @param pkt_serial frame serial.
   */
  int PushFrame(AVFrame *src_frame, double pts, double duration, int pkt_serial);

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

  void DumpDebugInformation();

};

#endif //FFP_SRC_RENDER_VIDEO_BASE_H_
