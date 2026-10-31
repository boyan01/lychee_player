#ifndef LYCHEE_PIPELINE_LIVE_DEMUXER_H_
#define LYCHEE_PIPELINE_LIVE_DEMUXER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/demuxer.h"
#include "pipeline/live_timeline.h"

namespace lychee {

// The input of lychee_player_open_live(): packets the caller pushes, one
// stream per track type, stamped with deadlines in the NowMicros()
// timebase by a LiveTimeline. The player's read thread reads them like any
// demuxer's.
//
// A track change or flush holds the stream: packets pushed from then on
// wait here until the player has replaced or flushed the renderer and
// calls Release(), so none reaches the old one and none is lost. Each hold
// is numbered: a Release() for a hold that a newer one replaced (the
// caller changed the track again meanwhile) leaves the stream held, and its
// packets, for the Release() of the newer one. Video
// packets are dropped until a keyframe after an open, a flush, a track
// change or an overflow; a keyframe without parameter sets gets the last
// ones seen, so a rebuilt decoder can start from it.
class LiveDemuxer : public Demuxer {
 public:
  static constexpr int kVideoStream = 0;
  static constexpr int kAudioStream = 1;

  // |audio_output_lead|: how far ahead of now the audio device consumes
  // audio, kept up to date by the audio renderer.
  LiveDemuxer(int64_t latency, const std::atomic<int64_t>* audio_output_lead);
  ~LiveDemuxer() override;

  // Demuxer. Open() succeeds at once with no streams; Seek() only clears
  // an Interrupt().
  const char* name() const override { return "live"; }
  bool Open(const std::string& url, std::string* error) override;
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>&) override {}
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return std::string(); }

  // Caller threads. Declares the codec of |stream|, or disables it if
  // |info.type| is kUnknown, and holds it. Queued packets are dropped.
  void SetTrack(int stream, const StreamInfo& info);
  // Drops |stream|'s queued packets and holds it. With both streams
  // flushed, the timeline anchors again on the next packet.
  void Flush(int stream);
  // False if |stream| has no codec; dropping a packet by policy (no
  // keyframe yet, overflow) still returns true. Copies |data|.
  bool Push(int stream, int64_t pts, const uint8_t* data, size_t size);

  // The number of |stream|'s latest hold. Read it, under the lock that
  // orders SetTrack() and Flush() calls, with the changes it covers.
  uint64_t hold(int stream) const;
  // Control thread, with the read thread parked. Lets the player read
  // |stream| again if |hold| is still its latest; |enabled| false drops its
  // packets until the next SetTrack(), for a stream no decoder took.
  void Release(int stream, bool enabled, uint64_t hold);
  // The video decoder was replaced: drop video up to the next keyframe.
  void RestartVideo();
  // Read thread: the packet Read() returned is queued for its decoder.
  void PacketDelivered();
  // Nothing readable is waiting here or on its way to a decoder. For
  // SettledForTesting().
  bool Idle() const;
  // Video packets dropped since the open while waiting for a keyframe
  // (after the open, a track change, a flush or a new decoder) and by an
  // overflow. Those a Flush() or SetTrack() discards are not counted: the
  // caller asked for that.
  int64_t video_packets_dropped() const;
  // LiveTimeline::audio_behind().
  int64_t audio_behind() const;

 private:
  struct Stream {
    bool has_codec = false;
    bool hevc = false;
    // Packets wait until Release().
    bool held = true;
    // Counts SetTrack() and Flush() calls.
    uint64_t hold = 0;
    // Packets are dropped (no decoder took the codec).
    bool disabled = false;
    bool wait_for_keyframe = true;
    std::deque<std::shared_ptr<EncodedPacket>> packets;
  };

  // |packet| is video in Annex B. Returns false to drop it.
  bool PrepareVideoLocked(Stream* stream, EncodedPacket* packet);
  void DropLocked(Stream* stream);
  bool ReadableLocked(const Stream& stream) const;

  const std::atomic<int64_t>* const audio_output_lead_;
  MediaInfo media_info_;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  LiveTimeline timeline_;
  Stream streams_[2];
  size_t bytes_ = 0;
  int64_t video_packets_dropped_ = 0;
  // Annex B VPS/SPS/PPS of the last video packet that had them.
  std::vector<uint8_t> parameter_sets_;
  // A packet Read() returned that is not queued for its decoder yet.
  bool in_flight_ = false;
  bool interrupted_ = false;
  bool aborted_ = false;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_LIVE_DEMUXER_H_
