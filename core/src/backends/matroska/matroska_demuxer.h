#ifndef LYCHEE_BACKENDS_MATROSKA_MATROSKA_DEMUXER_H_
#define LYCHEE_BACKENDS_MATROSKA_MATROSKA_DEMUXER_H_

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// Matroska and WebM (RFC 9559), for builds without FFmpeg. Video, audio
// and subtitle tracks; codec configuration is passed through as the
// container stores it (avcC, hvcC, av1C, OpusHead, the AudioSpecificConfig,
// FLAC's metadata blocks), with VP9's in the CodecPrivate and Colour
// elements. Seeks use the Cues, or an index of clusters it builds by
// skipping from one cluster header to the next when there are none.
class MatroskaDemuxer : public Demuxer {
 public:
  MatroskaDemuxer();
  ~MatroskaDemuxer() override;

  // The error of Open() for input that does not start like Matroska.
  static constexpr char kNotMatroska[] = "not a Matroska file";

  const char* name() const override { return "matroska"; }
  bool Open(const std::string& url, std::string* error) override;
  // Reads from |source| instead of opening the URL.
  bool Open(std::unique_ptr<DataSource> source, std::string* error);
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }

 private:
  struct Element;
  // kEnd: the data ended; kInvalid: it is not what it should be.
  enum class Status { kOk, kEnd, kError, kInvalid };

  struct Track {
    uint64_t number = 0;
    int stream = -1;
    bool enabled = true;
    // H.264 and HEVC may reorder frames; Matroska stores only their pts.
    bool needs_dts = false;
    // Nanoseconds; 0 if unknown.
    int64_t default_duration = 0;
    bool audio = false;
    // Microseconds the audio decoder outputs before the first sample:
    // block timestamps are later by as much.
    int64_t codec_delay = 0;
    // WebM's WebVTT (D_WEBVTT/...): each block starts with the cue's
    // identifier and settings lines, which are not text to show.
    bool webvtt_lines = false;
    // Header stripping (ContentCompAlgo 3): bytes every frame starts with.
    std::vector<uint8_t> stripped_header;
    // After a seek, packets up to the first keyframe are dropped.
    bool skip_to_keyframe = false;
    int64_t last_dts = kNoTimestamp;
  };
  struct CuePoint {
    int64_t time;  // Microseconds.
    uint64_t track;
    int64_t position;  // Absolute offset of the cluster.
  };
  struct ClusterEntry {
    int64_t position;
    int64_t time;  // Microseconds.
    // Where the next top-level element starts; -1 at the end.
    int64_t next;
  };
  struct Pending {
    EncodedPacket packet;
    // A packet whose dts is not known yet (Track::needs_dts).
    bool unresolved = false;
  };

  // Bytes at |offset|, through a small read-ahead buffer. Fewer than
  // |size| only at the end of the data; -1 on errors and interrupts.
  int64_t ReadAt(int64_t offset, uint8_t* data, size_t size);
  // The element header at |offset|. kEnd at the end of the data.
  Status ReadElementHeader(int64_t offset, Element* element);
  // An element's body, at most |limit| bytes.
  Status ReadBody(const Element& element,
                  size_t limit,
                  std::vector<uint8_t>* body,
                  std::string* error);

  bool ParseHeaders(std::string* error);
  void ParseSeekHead(const std::vector<uint8_t>& body);
  void ParseInfo(const std::vector<uint8_t>& body);
  bool ParseTracks(const std::vector<uint8_t>& body, std::string* error);
  bool ParseTrackEntry(const uint8_t* data, size_t size);
  void ParseTags(const std::vector<uint8_t>& body);
  void ParseAttachments(const std::vector<uint8_t>& body);
  void ParseCues(const std::vector<uint8_t>& body);
  // Reads the top-level element at |position| (from the SeekHead) if it is
  // one of the header elements, unread so far.
  void ReadElementAt(int64_t position);
  // Cues are read at the first seek: playback from the start needs none.
  void LoadCues();
  // Extends clusters_ up to the first cluster after |time|, or the end.
  void IndexClustersUpTo(int64_t time);

  // Reads elements until a packet is pending or the input ends.
  DemuxerReadResult ParseNext();
  // Queues the frames of a Block or SimpleBlock: the first |size| bytes
  // of |bytes|, which has kPacketPadding zero bytes more.
  void ParseBlock(const std::shared_ptr<std::vector<uint8_t>>& bytes,
                  size_t size,
                  bool simple,
                  bool reference,
                  int64_t duration_ticks,
                  int64_t discard_padding = 0);
  // Sets the dts of unresolved packets that can be resolved now; all of
  // them at the end of the input.
  void ResolveDts(bool all);
  // Microseconds from |ticks| in TimestampScale units.
  int64_t ToMicros(int64_t ticks) const;
  // An error "|what| at TIME s (of DURATION s)|detail|".
  DemuxerReadResult Fail(const char* what, const char* detail, int64_t time);
  Track* TrackByNumber(uint64_t number);
  void ClearPending();

  // Set once by Open(); the mutex is for Interrupt() and Abort() on other
  // threads meanwhile.
  std::mutex source_mutex_;
  std::unique_ptr<DataSource> source_;
  int64_t file_size_ = -1;
  std::vector<uint8_t> cache_;
  int64_t cache_offset_ = 0;

  MediaInfo media_info_;
  std::vector<Track> tracks_;
  int64_t timestamp_scale_ = 1000000;  // Nanoseconds per tick.
  int64_t segment_start_ = 0;          // Offset of the Segment's body.
  int64_t segment_end_ = -1;           // -1 if unknown.
  int64_t first_cluster_ = -1;
  // Header elements the SeekHead points at, absolute offsets; -1 if none.
  int64_t cues_position_ = -1;
  std::vector<int64_t> seek_positions_;
  bool cues_loaded_ = false;
  bool tags_read_ = false;
  bool attachments_read_ = false;
  std::vector<CuePoint> cues_;
  // Clusters seen while reading or indexing, by position.
  std::vector<ClusterEntry> clusters_;
  bool clusters_complete_ = false;

  // The read position: the next element, and the cluster it is in.
  int64_t position_ = 0;
  bool in_cluster_ = false;
  int64_t cluster_end_ = -1;  // -1: unknown size, ends at the next cluster.
  int64_t cluster_time_ = 0;  // Ticks.
  // Microseconds: where the blocks read so far end.
  int64_t last_time_ = 0;
  std::deque<Pending> pending_;
  // Set when the input ended: the result to report once |pending_| is out.
  bool input_done_ = false;
  DemuxerReadResult done_result_ = DemuxerReadResult::kEndOfStream;

  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MATROSKA_MATROSKA_DEMUXER_H_
