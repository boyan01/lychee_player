//
// Created by yangbin on 2021/2/13.
//

#ifndef MEDIA_PLAYER_MEDIA_PLAYER_H
#define MEDIA_PLAYER_MEDIA_PLAYER_H

#include <functional>
#include <memory>

#include "data_source.h"
#include "ffp_packet_queue.h"
#include "ffplayer.h"
#include "media_clock.h"
#include "render_audio_base.h"
#include "render_video_base.h"

enum class MediaPlayerState { IDLE = 0, READY, BUFFERING, END };

class MediaPlayer {
 private:
  std::shared_ptr<PacketQueue> audio_pkt_queue;
  std::shared_ptr<PacketQueue> video_pkt_queue;
  std::shared_ptr<PacketQueue> subtitle_pkt_queue;

  std::shared_ptr<MediaClock> clock_context;

  std::unique_ptr<DataSource> data_source{nullptr};

  std::shared_ptr<DecoderContext> decoder_context;

  std::shared_ptr<BasicAudioRender> audio_render_;
  std::shared_ptr<VideoRenderBase> video_render_;

  std::shared_ptr<MessageContext> message_context;

  MediaPlayerState player_state_ = MediaPlayerState::IDLE;
  std::mutex player_mutex_;

  std::function<void(int what, int64_t arg1, int64_t arg2)>
      message_callback_external_;

  double buffering_check_last_stamp_ = 0;

  bool play_when_ready_ = false;

  // buffered position in seconds. -1 if not available
  double buffered_position_ = -1;

 public:
  MediaPlayer(std::unique_ptr<VideoRenderBase> video_render,
              std::unique_ptr<BasicAudioRender> audio_render);

  ~MediaPlayer();

  static void GlobalInit();

 private:
  void DoSomeWork();

  void StopRenders();

  void StartRenders();

  void ChangePlaybackState(MediaPlayerState state);

  void PauseClock(bool pause);

  bool ShouldTransitionToReadyState(bool render_allow_play);

  void CheckBuffering();

 public:
  PlayerConfiguration start_configuration{};

  int OpenDataSource(const char* filename);

  double GetCurrentPosition();

  bool IsPlayWhenReady() const { return play_when_ready_; }

  void SetPlayWhenReady(bool play_when_ready);

  int GetVolume();

  void SetVolume(int volume);

  void SetMute(bool mute);

  bool IsMuted();

  double GetDuration();

  void Seek(double position);

  void SeekToChapter(int chapter);

  int GetCurrentChapter();

  int GetChapterCount();

  void SetMessageHandleCallback(
      std::function<void(int what, int64_t arg1, int64_t arg2)>
          message_callback);

  double GetVideoAspectRatio();

  const char* GetUrl();

  const char* GetMetadataDict(const char* key);

  VideoRenderBase* GetVideoRender();

  /**
   * Dump player status information to console.
   */
  void DumpStatus();
};

#endif  // MEDIA_PLAYER_MEDIA_PLAYER_H
