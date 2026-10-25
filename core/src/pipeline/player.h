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
#include "pipeline/subtitle_renderer.h"
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
  kTracksChanged = 7,
  kSubtitleChanged = 8,
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
  // Media read ahead of the playback position, per stream.
  int64_t video_buffered = 0;
  int64_t audio_buffered = 0;
  int64_t video_decode_time = 0;
};

// An audio, video or subtitle stream of the open media.
struct TrackInfo {
  MediaType type = MediaType::kUnknown;
  // The container's stream index.
  int stream_index = -1;
  bool selected = false;
  std::string codec;
  std::string language;
  std::string title;
  int width = 0;
  int height = 0;
  int sample_rate = 0;
  int channels = 0;
};

// Owns the demuxer and the renderers. The control thread opens the input,
// executes seeks and track changes and runs the state machine; the read
// thread only reads packets, so a read stalled on the network delays neither
// state changes nor seeks. Decoders run on the renderers' threads; video
// frames are pulled by the caller. Open() again replaces the whole pipeline.
class Player {
 public:
  using EventCallback =
      void (*)(void* user_data, int32_t type, int64_t arg0, int64_t arg1);

  static constexpr double kMinRate = 0.25;
  static constexpr double kMaxRate = 4.0;

  explicit Player(const PlayerConfig& config);
  ~Player();

  void SetEventCallback(EventCallback callback, void* user_data);

  // Stops whatever is open and opens |url|. Volume, rate, looping, the
  // event callback and whether to play are kept.
  bool Open(const std::string& url);
  void Play();
  void Pause();
  // |position| is relative to the start of the media.
  void Seek(int64_t position);
  void SetVolume(float volume);
  // lychee_player_set_video_output().
  void SetVideoOutput(void* output);
  // Clamped to [kMinRate, kMaxRate]. Pitch is preserved.
  void SetRate(double rate);
  // At the end, play again from the start instead of ending.
  void SetLooping(bool looping);
  // |track| indexes tracks(); -1 disables |type|. False if the request is
  // invalid; otherwise the change happens asynchronously and is reported by
  // kTracksChanged.
  bool SelectTrack(MediaType type, int track);

  PlayerState state() const;
  bool playing() const;
  double rate() const;
  bool looping() const;
  int64_t position() const;
  // kNoTimestamp if unknown.
  int64_t duration() const;
  int64_t buffered_position() const;
  bool video_size(int* width, int* height) const;
  int video_rotation() const;
  std::string error() const;
  PlayerStats stats() const;
  // Under virtual time (base/time.h): whether playback can move on to
  // system time |until| without waiting for data. False while loading,
  // buffering, seeking or switching tracks, until the control thread has
  // applied play/pause and end of stream, and while a renderer's decoded
  // data ends before |until|.
  bool SettledForTesting(int64_t until) const;
  std::vector<TrackInfo> tracks() const;
  // Container tag, case-insensitive. False if absent.
  bool metadata(const std::string& key, std::string* value) const;
  // The subtitle text showing at |display_time| (NowMicros() timebase), or
  // now if it is 0.
  std::string subtitle(int64_t display_time) const;

  bool HasNewFrame(int64_t display_time) const;
  std::shared_ptr<VideoFrame> AcquireFrame(int64_t display_time);

 private:
  struct Event {
    PlayerEvent type;
    int64_t arg0;
    int64_t arg1;
  };

  // A requested stream per type; kUnchanged if none was requested.
  static constexpr int kUnchanged = -2;
  struct TrackRequest {
    int video = kUnchanged;
    int audio = kUnchanged;
    int subtitle = kUnchanged;
    bool pending() const {
      return video != kUnchanged || audio != kUnchanged ||
             subtitle != kUnchanged;
    }
  };

  // Joins all threads and drops the renderers and the demuxer.
  void StopPipeline();
  void Run(std::string url);
  void ReadLoop();
  bool Prepare(const std::string& url, std::string* error);
  // Replaces the renderer of each type in |request|; stream -1 disables it.
  // The read thread must be parked.
  void ChangeTracks(const TrackRequest& request);
  std::unique_ptr<VideoRenderer> CreateVideoRenderer(const StreamInfo& info);
  std::unique_ptr<AudioRenderer> CreateAudioRenderer(const StreamInfo& info);
  std::unique_ptr<SubtitleRenderer> CreateSubtitleRenderer(
      const StreamInfo& info);
  void EnableStreamsLocked();
  void PublishVideoLocked();
  bool ShouldReadLocked() const;
  bool NeedMoreData() const;
  void ReadPacket();
  // |loop|: back to the start at the end; the state stays kReady.
  void ProcessSeek(int64_t target, bool loop);
  void FinishSeekLocked();
  void UpdateState();
  void UpdateVideoSizeLocked();
  void UpdateSubtitleLocked();
  void ApplyPlayingLocked();
  bool PrerollDoneLocked() const;
  bool EndedLocked() const;
  bool StarvedLocked() const;
  void SetStateLocked(PlayerState state);
  void PostEventLocked(PlayerEvent type, int64_t arg0 = 0, int64_t arg1 = 0);
  void DispatchEvents();
  void FailLocked(const std::string& error);
  // The streams of |media_info_| exposed as tracks.
  std::vector<int> TrackStreamsLocked() const;

  const PlayerConfig config_;
  MediaClock clock_;
  PacketQueue video_queue_;
  PacketQueue audio_queue_;
  // Serializes Open() calls with each other and with the destructor.
  std::mutex open_mutex_;
  std::unique_ptr<Demuxer> demuxer_;
  std::unique_ptr<VideoRenderer> video_renderer_;
  std::unique_ptr<AudioRenderer> audio_renderer_;
  std::unique_ptr<SubtitleRenderer> subtitle_renderer_;
  std::thread thread_;
  std::thread read_thread_;

  // The renderer frames are pulled from. Its own lock, so a renderer can be
  // replaced while the caller pulls frames on another thread. Lock order:
  // display_mutex_ before the renderer's.
  mutable std::mutex display_mutex_;
  VideoRenderer* video_for_display_ = nullptr;

  // Written under |mutex_| while the read thread is parked or not started;
  // the read thread reads them without the lock.
  int video_stream_ = -1;
  int audio_stream_ = -1;
  int subtitle_stream_ = -1;
  // Control thread only.
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
  // The pending seek came from Seek(), so its completion is reported.
  bool report_seek_ = false;
  // Whether the seek in progress reports kSeekCompleted. Loops and the
  // resync after a track change do not.
  bool current_seek_reported_ = false;
  // The control thread is seeking the demuxer or changing tracks; the read
  // thread stays out.
  bool seeking_ = false;
  // The read thread is inside Demuxer::Read().
  bool reading_ = false;
  bool end_of_input_ = false;
  // Buffering because playback ran out of data, not because of a seek or
  // the start: resume only with full read-ahead, so a slow network plays in
  // long stretches instead of stuttering.
  bool rebuffering_ = false;
  // Looping back to the start: the state stays kReady, but the clock waits
  // for the renderers to preroll.
  bool loop_seek_pending_ = false;
  bool prerolling_ = false;
  TrackRequest track_request_;
  // The input stopped early (network failure, truncated file). Reported
  // once the data read so far has played, unless a seek comes first.
  std::string input_error_;
  float volume_ = 1.0f;
  // RetainVideoOutput() of the last SetVideoOutput().
  std::shared_ptr<void> video_output_;
  double rate_ = 1.0;
  bool looping_ = false;
  std::string error_;
  MediaInfo media_info_;
  int64_t video_last_pts_ = kNoTimestamp;
  // Display size last reported by kVideoSizeChanged.
  int video_width_ = 0;
  int video_height_ = 0;
  int64_t audio_last_pts_ = kNoTimestamp;
  // Subtitle text last reported by kSubtitleChanged.
  std::string subtitle_text_;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_PLAYER_H_
