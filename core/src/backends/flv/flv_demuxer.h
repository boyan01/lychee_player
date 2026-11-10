#ifndef LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_
#define LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/legacy/indexed_demuxer.h"

namespace lychee {

// Adobe FLV 10: AAC audio; AVC video, HEVC as codec id 12 (what Chinese
// live services send), and Enhanced RTMP's FourCC video headers (avc1,
// hvc1, av01; veovera/enhanced-rtmp).
//
// Input of a known size (a file, an HTTP response with a length) is
// indexed at the open, like the other legacy containers, and seeks.
// Input of unknown size (an HTTP-FLV live stream, a chunked response) is
// read as it arrives: the open reads only until the streams'
// configurations are in, and it cannot seek. Without a duration in its
// onMetaData it counts as a live stream (MediaInfo::realtime): its
// timestamps are kept on one timeline across jumps (an encoder restart,
// a new connection), and a connection that breaks is opened again
// (SetReopener()).
class FlvDemuxer final : public IndexedDemuxer {
 public:
  const char* name() const override { return "flv"; }
  static bool Detect(const uint8_t* data, size_t size) {
    return size >= 9 && memcmp(data, "FLV", 3) == 0 && data[3] == 1;
  }

  // Before Open(): how to open the input, |url|, again once its
  // connection broke.
  void SetReopener(DataSourceOpener opener, std::string url) {
    reopener_ = std::move(opener);
    url_ = std::move(url);
  }

  DemuxerReadResult Read(EncodedPacket* packet) override;
  void Interrupt() override;

  // Timestamps further than these from the newest one read start a new
  // timeline: behind it (an encoder restart; FLV's 32-bit milliseconds
  // wrapping) or ahead of it (a gap the stream did not play through).
  static constexpr int64_t kMaxBackwardJump = 1000000;
  static constexpr int64_t kMaxForwardJump = 3000000;
  // A new connection whose first frame is at most this far behind the
  // newest one read is on the same timeline: the server sends again what
  // it still holds (a CDN its last GOP), which is dropped up to what was
  // read. Otherwise the stream goes on after what was read.
  static constexpr int64_t kMaxResend = 10000000;
  // Waits before each attempt to reconnect in a row, in seconds.
  static constexpr int kReconnectDelays[] = {0, 1, 2, 4, 8};

 private:
  // What a tag's payload holds, from its first bytes.
  struct Payload {
    enum Kind { kSkip, kConfig, kFrame, kMetadata } kind = kSkip;
    MediaType type = MediaType::kVideo;
    VideoCodec codec = VideoCodec::kUnknown;
    // Bytes before the configuration or the frame's data.
    size_t skip = 0;
    int64_t cts = 0;
    bool key = true;
  };
  // kError: the input broke (a reconnect may help); kInvalid: what it
  // holds is not something this reader plays.
  enum class Step { kOk, kEnd, kError, kInvalid, kAborted };

  bool Parse() override;
  bool ParseIndexed(int64_t offset);
  bool OpenStream();

  // False with |error_| set if the tag is malformed or of a codec this
  // reader lacks.
  bool Inspect(uint8_t type, const uint8_t* head, uint32_t size, Payload* out);
  // A sequence header (configuration record) of |payload|'s stream.
  bool AddConfig(const Payload& payload, std::vector<uint8_t> config);
  void ParseMetadata(const uint8_t* data, size_t size);
  // Fills in what the configurations lack (AV1's picture size).
  void FinishStreams();

  // Streaming: bytes read from the source and not yet parsed.
  Step Need(size_t size);
  // The FLV header and the first previous-tag size.
  Step ReadHeader();
  // One tag; a frame goes into |pending_|.
  Step ReadTag();
  // Opens the input again, from where the attempts in a row left off;
  // kEnd if that fails after a clean end.
  Step Reconnect();
  // A tag's |time| on the timeline packets leave on; with |jump|, it
  // starts a new one.
  int64_t Rebase(int stream, int64_t time, bool jump);

  // The header's flags: 4 audio, 1 video.
  uint8_t flags_ = 0;
  int video_ = -1;
  int audio_ = -1;
  // From onMetaData; 0 if absent.
  double metadata_duration_ = 0;
  int metadata_width_ = 0;
  int metadata_height_ = 0;

  DataSourceOpener reopener_;
  std::string url_;
  std::vector<uint8_t> buffer_;
  size_t buffer_head_ = 0;
  int64_t read_offset_ = 0;
  std::deque<EncodedPacket> pending_;
  bool ended_ = false;
  // Added to tag times, and the newest time read with it.
  int64_t timeline_offset_ = 0;
  int64_t newest_ = kNoTimestamp;
  // A new connection: its first frame decides where it goes on.
  bool resumed_ = false;
  // Per stream: the last frame's time, the time between frames, and
  // whether frames up to the last one are being sent again.
  std::vector<int64_t> last_dts_;
  std::vector<int64_t> step_;
  std::vector<bool> resending_;
  // The connection broke: Read() opens it again first. After a clean
  // end, which some clients also report for a connection cut off (Apple's
  // for a chunked response), or after a reconnect that brought no new
  // frame, once only, and failing that it ends.
  bool broken_ = false;
  bool after_end_ = false;
  size_t reconnects_ = 0;
  std::mutex wait_mutex_;
  std::condition_variable wait_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FLV_FLV_DEMUXER_H_
