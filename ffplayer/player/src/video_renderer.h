//
// Created by yangbin on 2021/5/5.
//

#ifndef MEDIA_PLAYER_SRC_VIDEO_RENDERER_H_
#define MEDIA_PLAYER_SRC_VIDEO_RENDERER_H_

#include <ostream>

#include "base/task_runner.h"
#include "decoder_stream.h"
#include "demuxer_stream.h"
#include "media_clock.h"
#include "video_renderer_sink.h"

namespace media {

class VideoRenderer : public VideoRendererSink::RenderCallback,
                      public std::enable_shared_from_this<VideoRenderer> {
 public:
  VideoRenderer(const TaskRunner &media_task_runner,
                std::shared_ptr<TaskRunner> decode_task_runner,
                std::shared_ptr<VideoRendererSink> video_renderer_sink);

  ~VideoRenderer() override;

  using InitCallback = std::function<void(bool success)>;

  void Initialize(DemuxerStream *stream,
                  std::shared_ptr<MediaClock> media_clock,
                  InitCallback init_callback);

  std::shared_ptr<VideoFrame> Render(TimeDelta &next_frame_delay) override;

  void OnFrameDrop() override;

  void Start();

  void Stop();

  VideoRendererSink *video_renderer_sink() { return sink_.get(); }

  void Flush();

  void MarkStatePlaying();

  friend std::ostream &operator<<(std::ostream &os,
                                  const VideoRenderer &renderer);

 private:
  enum State { kUnInitialized, kInitializing, kFlushing, kFlushed, kPlaying };
  State state_ = kUnInitialized;

  TaskRunner media_task_runner_;
  std::shared_ptr<TaskRunner> decode_task_runner_;

  std::shared_ptr<VideoRendererSink> sink_;

  std::shared_ptr<VideoDecoderStream> decoder_stream_;

  std::deque<std::shared_ptr<VideoFrame>> ready_frames_;

  std::shared_ptr<MediaClock> media_clock_;

  InitCallback init_callback_;

  std::mutex _render_mutex;

  int frame_drop_count_ = 0;

  bool reading_ = false;

  void OnDecodeStreamInitialized(bool success);

  void AttemptReadFrame();

  bool CanDecodeMore();

  void OnNewFrameAvailable(std::shared_ptr<VideoFrame> frame);

  // To get current clock time in seconds.
  double GetDrawingClock();

  DELETE_COPY_AND_ASSIGN(VideoRenderer);
};

}  // namespace media

#endif  // MEDIA_PLAYER_SRC_VIDEO_RENDERER_H_
