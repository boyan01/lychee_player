//
// Created by yangbin on 2021/2/13.
//

#ifndef MEDIA_PLAYER_MEDIA_PLAYER_H
#define MEDIA_PLAYER_MEDIA_PLAYER_H

#include <functional>
#include <memory>
#include <utility>

#include "audio_decoder.h"
#include "audio_renderer.h"
#include "base/task_runner.h"
#include "data_source.h"
#include "decoder_stream.h"
#include "demuxer.h"
#include "ffplayer.h"
#include "media_clock.h"
#include "video_renderer.h"

namespace media {

enum class MediaPlayerState { IDLE = 0, READY, BUFFERING, END };

class MediaPlayer : public std::enable_shared_from_this<MediaPlayer>,
                    public DemuxerHost {
 public:
  /**
   * Global init player resources.
   */
  static void GlobalInit();

  MediaPlayer(std::unique_ptr<VideoRendererSink> video_renderer_sink,
              std::shared_ptr<AudioRendererSink> audio_renderer_sink,
              const TaskRunner &task_runner);

  ~MediaPlayer() override;

 private:
  enum State { kUninitialized, kIdle, kPrepared, kPreparing };

  State state_ = kUninitialized;

  std::shared_ptr<MediaClock> clock_context;

  std::shared_ptr<Demuxer> demuxer_;

  std::shared_ptr<AudioRenderer> audio_renderer_;
  std::shared_ptr<VideoRenderer> video_renderer_;

  MediaPlayerState player_state_ = MediaPlayerState::IDLE;
  std::mutex player_mutex_;

  double buffering_check_last_stamp_ = 0;

  bool play_when_ready_ = false;
  bool play_when_ready_pending_ = false;

  // buffered position in seconds. -1 if not available
  double buffered_position_ = -1;

  void Initialize();

  void StopRenders();

  void StartRenders();

  void ChangePlaybackState(MediaPlayerState state);

  void PauseClock(bool pause);

  void OpenDataSourceTask(const char *filename);

  void OnDataSourceOpen(int open_status);

  void InitVideoRender();

  void OnVideoRendererInitialized(bool success);

  void InitAudioRender();

  void OnAudioRendererInitialized(bool success);

  void SeekInternal(TimeDelta position);

 public:
  void SetDuration(double duration) override;
  void OnDemuxerError(PipelineStatus error) override;

 public:
  PlayerConfiguration start_configuration{};

  int OpenDataSource(const char *filename);

  TimeDelta GetCurrentPosition();

  bool IsPlayWhenReady() const { return play_when_ready_; }

  void SetPlayWhenReady(bool play_when_ready);

  double GetVolume();

  void SetVolume(double volume);

  double GetDuration() const;

  void Seek(TimeDelta position);

  VideoRendererSink *GetVideoRenderSink() {
    return video_renderer_->video_renderer_sink();
  }

  /**
   * Dump player status information to console.
   */
  void DumpStatus();

  using OnVideoSizeChangeCallback = std::function<void(int width, int height)>;
  void set_on_video_size_changed_callback(OnVideoSizeChangeCallback callback) {
    on_video_size_changed_ = std::move(callback);
  }

 private:
  TaskRunner task_runner_;

  OnVideoSizeChangeCallback on_video_size_changed_;

  double duration_ = -1;

  void OnFirstFrameLoaded(int width, int height);

  void OnFirstFrameRendered(int width, int height);

  void DumpMediaClockStatus();

  void OnSeekCompleted(bool succeed);

  void SetPlayWhenReadyTask(bool play_when_ready);
};

}  // namespace media

#endif  // MEDIA_PLAYER_MEDIA_PLAYER_H
