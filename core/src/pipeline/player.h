#ifndef LYCHEE_PIPELINE_PLAYER_H_
#define LYCHEE_PIPELINE_PLAYER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/demuxer.h"
#include "pipeline/audio_renderer.h"
#include "pipeline/media_clock.h"
#include "pipeline/packet_queue.h"
#include "pipeline/video_renderer.h"

namespace lychee {

enum class PlayerState {
  kIdle = 0,
  kLoading = 1,
  kReady = 2,
  kBuffering = 3,
  kEnded = 4,
  kError = 5,
};

// Values match lychee_event_type.
enum class PlayerEvent {
  kStateChanged = 1,
  kPlayingChanged = 2,
  kMediaInfo = 3,
  kVideoSizeChanged = 4,
  kSeekCompleted = 5,
  kError = 6,
};

struct PlayerConfig {
  uint32_t video_frame_types = 0;
  bool hardware_decoding = true;
};

struct PlayerStats {
  const char* video_decoder = "none";
  bool hardware = false;
  int64_t frames_rendered = 0;
  int64_t frames_dropped = 0;
};

// Owns the demuxer and the renderers. The demux thread reads packets,
// executes seeks and runs the state machine; decoders run on the renderers'
// threads; video frames are pulled by the caller.
class Player {
 public:
  using EventCallback =
      void (*)(void* user_data, int32_t type, int64_t arg0, int64_t arg1);

  explicit Player(const PlayerConfig& config);
  ~Player();

  void SetEventCallback(EventCallback callback, void* user_data);

  bool Open(const std::string& url);
  void Play();
  void Pause();
  // |position| is relative to the start of the media.
  void Seek(int64_t position);
  void SetVolume(float volume);

  PlayerState state() const;
  bool playing() const;
  int64_t position() const;
  // kNoTimestamp if unknown.
  int64_t duration() const;
  int64_t buffered_position() const;
  bool video_size(int* width, int* height) const;
  int video_rotation() const;
  std::string error() const;
  PlayerStats stats() const;

  bool HasNewFrame(int64_t display_time) const;
  std::shared_ptr<VideoFrame> AcquireFrame(int64_t display_time);

 private:
  struct Event {
    PlayerEvent type;
    int64_t arg0;
    int64_t arg1;
  };

  void Run(std::string url);
  bool Prepare(const std::string& url, std::string* error);
  bool NeedMoreData() const;
  void ReadPacket();
  void ProcessSeek(int64_t target);
  void UpdateState();
  void ApplyPlayingLocked();
  bool PrerollDoneLocked() const;
  bool EndedLocked() const;
  bool StarvedLocked() const;
  void SetStateLocked(PlayerState state);
  void PostEventLocked(PlayerEvent type, int64_t arg0 = 0, int64_t arg1 = 0);
  void DispatchEvents();
  void FailLocked(const std::string& error);

  const PlayerConfig config_;
  MediaClock clock_;
  PacketQueue video_queue_;
  PacketQueue audio_queue_;
  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<VideoRenderer> video_renderer_;
  std::unique_ptr<AudioRenderer> audio_renderer_;
  // Published once prepared, read from the frame-pulling threads.
  std::atomic<VideoRenderer*> video_for_display_{nullptr};
  std::thread thread_;

  // Demux thread only.
  int video_stream_ = -1;
  int audio_stream_ = -1;
  bool end_of_input_ = false;
  uint64_t generation_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  EventCallback event_callback_ = nullptr;
  void* event_user_data_ = nullptr;
  std::vector<Event> events_;
  PlayerState state_ = PlayerState::kIdle;
  bool play_when_ready_ = false;
  bool clock_running_ = false;
  bool quit_ = false;
  bool seek_pending_ = false;
  bool seek_in_progress_ = false;
  int64_t seek_target_ = 0;
  float volume_ = 1.0f;
  std::string error_;
  MediaInfo media_info_;
  int64_t video_last_pts_ = kNoTimestamp;
  int64_t audio_last_pts_ = kNoTimestamp;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_PLAYER_H_
