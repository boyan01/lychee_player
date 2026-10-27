#ifndef LYCHEE_PIPELINE_PLAYER_H_
#define LYCHEE_PIPELINE_PLAYER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/demuxer.h"
#include "pipeline/audio_renderer.h"
#include "pipeline/live_demuxer.h"
#include "pipeline/media_clock.h"
#include "pipeline/packet_history.h"
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
  // 0: audio only.
  uint32_t video_frame_types = 0;
  bool hardware_decoding = true;
  VideoDevice video_device;
  // Android: MediaCodec rotates surface frames upright.
  bool rotate_surface_frames = false;
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
//
// OpenLive() reads packets the caller pushes instead (LiveDemuxer): the
// media timeline is then the NowMicros() timebase, the clock runs from the
// open on, packet timestamps are deadlines, and there is no buffering,
// seeking, rate or end. Track changes and flushes park the read thread
// like a seek, but replace or flush only the renderer concerned.
class Player {
 public:
  using EventCallback =
      void (*)(void* user_data, int32_t type, int64_t arg0, int64_t arg1);

  static constexpr double kMinRate = 0.25;
  static constexpr double kMaxRate = 4.0;

  explicit Player(const PlayerConfig& config);
  ~Player();

  // Returns once no thread is in the old callback, unless called from it.
  void SetEventCallback(EventCallback callback, void* user_data);

  // Stops whatever is open and opens |url|. Volume, rate, looping, the
  // event callback and whether to play are kept.
  bool Open(const std::string& url);
  // Stops whatever is open and starts a live input presenting packets
  // |latency| after they arrive (lychee_player_open_live()).
  bool OpenLive(int64_t latency);
  // The codec of the live |type| track, or none if |info.type| is
  // kUnknown. False if no live input is open.
  bool SetLiveTrack(MediaType type, const StreamInfo& info);
  // False if no live input is open or |type| has no codec.
  bool PushLivePacket(MediaType type,
                      int64_t pts,
                      const uint8_t* data,
                      size_t size);
  // kUnknown flushes both tracks and anchors the timeline again.
  bool FlushLive(MediaType type);
  void Play();
  void Pause();
  // |position| is relative to the start of the media.
  void Seek(int64_t position);
  void SetVolume(float volume);
  // Rebuilds the video renderer on |device| during playback.
  void SetVideoDevice(VideoDevice device);
  // lychee_player_set_video_surface().
  void SetVideoSurface(void* surface);
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
  // Whether a seek can move playback; false until the input is open.
  bool seekable() const;
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
  // data ends before |until|, or while the callback has yet to see an
  // event. Live: LiveSettledLocked().
  bool SettledForTesting(int64_t until) const;
  // Makes the video decoder count as lost, as after a GPU reset.
  void LoseVideoDecoderForTesting();
  // Makes the video decoder hang, as a driver that never returns.
  void HangVideoDecoderForTesting();
  std::vector<TrackInfo> tracks() const;
  // Container tag, case-insensitive. False if absent.
  bool metadata(const std::string& key, std::string* value) const;
  // The cover picture, encoded; null if the media has none.
  std::shared_ptr<const std::vector<uint8_t>> artwork() const;
  // The subtitle text showing at |display_time| (NowMicros() timebase), or
  // now if it is 0.
  std::string subtitle(int64_t display_time) const;
  // The subtitle pictures showing at |display_time| (0 for now) on a
  // |width| x |height| canvas (0 for the video's size); null if none.
  std::shared_ptr<const SubtitleImages> subtitle_images(int64_t display_time,
                                                        int width,
                                                        int height);

  bool HasNewFrame(int64_t display_time) const;
  std::shared_ptr<VideoFrame> AcquireFrame(int64_t display_time);
  // |pts| of a frame from AcquireFrame() on the file's own timeline: after
  // a loop, frames carry the time of the pass they belong to.
  int64_t UnloopedPts(int64_t pts) const;

 private:
  // True, and logs why, on this player's control thread: Open() there
  // would join the thread it runs on.
  bool OnControlThread() const;
  struct Event {
    PlayerEvent type;
    int64_t arg0;
    int64_t arg1;
  };

  // A requested stream per type; kUnchanged if none was requested.
  static constexpr int kUnchanged = -2;
  // Live track changes and flushes the caller asked for, per
  // LiveDemuxer stream.
  struct LiveChange {
    bool track[2] = {false, false};
    StreamInfo info[2];
    bool flush[2] = {false, false};
    // LiveDemuxer::hold() of each stream when the change was taken.
    uint64_t hold[2] = {0, 0};
    bool pending() const {
      return track[0] || track[1] || flush[0] || flush[1];
    }
  };

  struct TrackRequest {
    int video = kUnchanged;
    int audio = kUnchanged;
    // Replace the video renderer even if its stream stays: for a new
    // device, or a lost decoder.
    bool rebuild_video = false;
    // Seek back to the clock's position even if no track changes: a
    // cancelled request already interrupted the read.
    bool resync = false;
    bool tracks_changed() const {
      return video != kUnchanged || audio != kUnchanged;
    }
    bool pending() const {
      return tracks_changed() || rebuild_video || resync;
    }
  };

  // Joins all threads and drops the renderers and the demuxer.
  void StopPipeline();
  void Run(std::string url);
  void ReadLoop();
  bool Prepare(const std::string& url, std::string* error);
  // Replaces the renderer of each type in |request|; stream -1 disables it.
  // Rebuilds the video renderer of the current stream for |rebuild_video|.
  // The read thread must be parked.
  void ChangeTracks(const TrackRequest& request);
  // Live: replaces and flushes renderers for |request| and |change|. The
  // read thread must be parked.
  void ApplyLiveChange(TrackRequest request, const LiveChange& change);
  // Live: everything pushed so far has been decoded; for
  // SettledForTesting().
  bool LiveSettledLocked() const;
  VideoRenderer::Ptr CreateVideoRenderer(const StreamInfo& info);
  std::unique_ptr<AudioRenderer> CreateAudioRenderer(const StreamInfo& info);
  // The renderer of |subtitle_stream_|, or null.
  SubtitleRenderer* SelectedSubtitleLocked() const;
  std::unique_ptr<SubtitleRenderer> CreateSubtitleRenderer(
      const StreamInfo& info,
      const std::vector<SubtitleFont>& fonts);
  void EnableStreamsLocked();
  void PublishVideoLocked();
  // A new video renderer is needed: its decoder was lost, or the device
  // changed.
  void CheckVideoRendererLocked();
  bool ShouldReadLocked() const;
  bool NeedMoreData() const;
  void ReadPacket();
  // Read thread, at the end of the input while looping: seeks the demuxer
  // back to the start and marks the boundary in the packet queues, so
  // playback runs on without stopping. False if it cannot; the control
  // thread then loops by seeking.
  bool WrapLoop();
  // Media time of the start of the pass playing at |media_time|, relative
  // to the first pass.
  int64_t LoopOffsetLocked(int64_t media_time) const;
  // Looping off after the read thread wrapped past the pass playing: the
  // renderers end at that pass's end instead of playing the next.
  void ApplyLoopEndLocked();
  // |loop|: back to the start at the end; the state stays kReady.
  // |resync|: after a track change, playing on from where playback is, in
  // pass |loops|.
  void ProcessSeek(int64_t target, bool loop, bool resync, int64_t loops,
                   bool tracks_changed);
  void FinishSeekLocked();
  void UpdateState();
  void UpdateVideoSizeLocked();
  // The display size of the frame on screen, false before one is shown.
  bool ShownSizeLocked(int* width, int* height) const;
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
  // Set with |demuxer_| under both locks: whether it is a LiveDemuxer, and
  // that one.
  bool live_ = false;
  LiveDemuxer* live_demuxer_ = nullptr;
  // Written by the live audio renderer, read by |live_demuxer_|.
  std::atomic<int64_t> audio_output_lead_{0};
  VideoRenderer::Ptr video_renderer_;
  std::unique_ptr<AudioRenderer> audio_renderer_;
  // One per subtitle stream that has a decoder, from Prepare() until
  // StopPipeline(): every subtitle stream is read, so a track switched on
  // midway already has the cues read so far. The map itself does not
  // change in between, so the read thread uses it without the lock.
  std::map<int, std::unique_ptr<SubtitleRenderer>> subtitle_renderers_;
  // Input that cannot seek (not live): one per video and audio stream, from
  // Prepare() until StopPipeline(). A resync replays them in place of a
  // seek. Every one of those streams is read (in HLS, video only once
  // selected), so one switched to midway starts at the position too. One not
  // selected costs its packets from the keyframe before the position to the
  // read position (a GOP and the read-ahead), up to kMaxQueuedBytes. The map
  // does not change in between; the read thread pushes to it, and the control
  // thread uses it while the read thread is parked.
  std::map<int, PacketHistory> histories_;
  std::thread thread_;
  std::thread read_thread_;

  // The renderer frames are pulled from: |video_renderer_|, unless it was
  // made for a device other than |video_device_|. Its own lock, so a
  // renderer can be replaced while the caller pulls frames on another
  // thread. Lock order: display_mutex_ before the renderer's.
  mutable std::mutex display_mutex_;
  VideoRenderer* video_for_display_ = nullptr;

  // Written under |mutex_| while the read thread is parked or not started;
  // the read thread reads them without the lock.
  int video_stream_ = -1;
  int audio_stream_ = -1;
  // Under |mutex_|; the read thread does not use it.
  int subtitle_stream_ = -1;
  // Control thread only.
  uint64_t generation_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  EventCallback event_callback_ = nullptr;
  void* event_user_data_ = nullptr;
  std::vector<Event> events_;
  // DispatchEvents() is handing events to the callback.
  bool dispatching_ = false;
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
  // Times the read thread wrapped around since the last seek. Packets of
  // pass N are read with timestamps N durations later, so the clock and
  // renderers see one timeline that keeps going (WrapLoop()). Written
  // under |mutex_| by the read thread, or by the control thread while it is
  // parked; the read thread reads it without the lock.
  int64_t loops_ = 0;
  // ApplyLoopEndLocked(): where playback ends instead of looping, or
  // kNoTimestamp.
  int64_t loop_end_ = kNoTimestamp;
  TrackRequest track_request_;
  LiveChange live_change_;
  // The input stopped early (network failure, truncated file). Reported
  // once the data read so far has played, unless a seek comes first.
  std::string input_error_;
  float volume_ = 1.0f;
  VideoDevice video_device_;
  // RetainVideoSurface() of the last SetVideoSurface().
  std::shared_ptr<void> video_surface_;
  // Video decoders lost since the last open or device change. The first is
  // replaced by hardware decoding again (a GPU reset passes), later ones by
  // software decoding. A hung decoder counts as two.
  int video_decoder_losses_ = 0;
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
  uint64_t subtitle_images_key_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_PLAYER_H_
