//
// Created by yangbin on 2021/2/13.
//

#ifndef MEDIA_PLAYER_MEDIA_PLAYER_H
#define MEDIA_PLAYER_MEDIA_PLAYER_H

#include <memory>
#include <functional>
#include <utility>

#include "ffplayer.h"
#include "media_clock.h"
#include "data_source.h"
#include "task_runner.h"
#include "audio_renderer.h"
#include "video_renderer.h"
#include "audio_decoder.h"
#include "decoder_stream.h"
#include "demuxer.h"

namespace media {

enum class MediaPlayerState {
  IDLE = 0,
  READY,
  BUFFERING,
  END
};

class MediaPlayer : public std::enable_shared_from_this<MediaPlayer>, public DemuxerHost {

 public:

  /**
   * Global init player resources.
   */
  static void GlobalInit();

  MediaPlayer(std::unique_ptr<VideoRendererSink> video_renderer_sink,
              std::shared_ptr<AudioRendererSink> audio_renderer_sink);

  ~MediaPlayer();

 private:

  enum State {
    kUninitialized,
    kIdle,
    kPrepared,
    kPreparing
  };

  State state_ = kUninitialized;

  std::shared_ptr<MediaClock> clock_context;

  std::shared_ptr<Demuxer> demuxer_;

  std::shared_ptr<AudioRenderer> audio_renderer_;
  std::shared_ptr<VideoRenderer> video_renderer_;

  MediaPlayerState player_state_ = MediaPlayerState::IDLE;
  std::mutex player_mutex_;

  std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback_external_;

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

 public:
  void SetDuration(double duration) override;
  void OnDemuxerError(PipelineStatus error) override;

 public:
  PlayerConfiguration start_configuration{};

  int OpenDataSource(const char *filename);

  double GetCurrentPosition();

  bool IsPlayWhenReady() const { return play_when_ready_; }

  void SetPlayWhenReady(bool play_when_ready);

  void SetPlayWhenReadyTask(bool play_when_ready);

  double GetVolume();

  void SetVolume(double volume);

  double GetDuration();

  void Seek(double position);

  void SeekToChapter(int chapter);

  int GetCurrentChapter();

  int GetChapterCount();

  void SetMessageHandleCallback(std::function<void(int what, int64_t arg1, int64_t arg2)> message_callback);

  const char *GetUrl();

  const char *GetMetadataDict(const char *key);

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

  TaskRunner *task_runner_;
  TaskRunner *decoder_task_runner_;

  OnVideoSizeChangeCallback on_video_size_changed_;

  void OnFirstFrameLoaded(int width, int height);

  void OnFirstFrameRendered(int width, int height);

  void DumpMediaClockStatus();

};

} // namespace media

#endif //MEDIA_PLAYER_MEDIA_PLAYER_H
