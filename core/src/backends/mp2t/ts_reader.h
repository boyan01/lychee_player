#ifndef LYCHEE_BACKENDS_MP2T_TS_READER_H_
#define LYCHEE_BACKENDS_MP2T_TS_READER_H_

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "backends/mp2t/ts_parser.h"
#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// A stream of the program the reader plays.
struct TsStream {
  int pid = -1;
  // index is the position in TsReader::streams().
  StreamInfo info;
  // Video: an SPS was seen (size known). Audio: a frame header was seen.
  bool configured = false;
  // The earliest timestamp read so far, microseconds.
  int64_t first_pts = kNoTimestamp;
};

// A run of timestamps in one input: where they start over (files joined
// without remuxing, spliced recordings), each later run is shifted so
// that its first frame follows the last one of the run before it. Frames
// of the anchor stream (video if there is any, else audio) decide, since
// video gaps show; other streams may overlap or leave a gap of a frame.
struct TsRun {
  // Where the run's first PES packet starts.
  int64_t offset = 0;
  // A timestamp in it as read (any 33-bit unwrapping), microseconds: where
  // its timestamps unwrap from.
  int64_t start = kNoTimestamp;
  // Added to its timestamps.
  int64_t shift = 0;
  // Whether |shift| was set from the anchor stream's first frame.
  bool anchored = true;
  // The latest end of an anchor stream frame read in it, after the shift.
  int64_t end = kNoTimestamp;
};
// Runs in input order; the first starts at offset 0 with no shift.
using TsTimeline = std::vector<TsRun>;

// Reads MPEG-TS from a DataSource into packets: one access unit per
// video packet (Annex B, as the stream carries it), one ADTS frame per
// AAC packet (header included). Timestamps are microseconds, unwrapped
// across the 33-bit wraparound; each packet's duration is the distance
// to the next one of its stream.
//
// Streams are the program's playable ones (H.264, HEVC, AAC, MP3,
// AC-3, E-AC-3) in PMT order. A new PMT version keeps a stream's index
// when its PID stays, or when a new PID of the same type replaces one
// that is gone (a rendition switch).
class TsReader : private TsParser::Client {
 public:
  explicit TsReader(DataSource* source);
  ~TsReader() override;

  // Whether |data| (the input's first bytes) looks like MPEG-TS: sync
  // bytes 188 (or 192, M2TS) bytes apart.
  static bool Detect(const uint8_t* data, size_t size);

  // Reads the source's bytes [begin, end) (|end| -1: to its end) from
  // |begin| on. Call before anything else.
  void SetRange(int64_t begin, int64_t end);

  // Reads until the program, each stream's configuration and its first
  // timestamp are known, or |max_bytes| are read. Packets read meanwhile
  // are kept for Read(). False if there is no program (or a read failed).
  bool Probe(int64_t max_bytes, std::string* error);

  // The next packet; its stream_index is into streams(), |*offset| where
  // its PES packet starts in the input.
  DemuxerReadResult Read(EncodedPacket* packet, int64_t* offset);

  // Reads from |offset| on, keeping the program. Timestamps unwrap next
  // to |reference| (microseconds), or are taken as they are if it is
  // kNoTimestamp.
  void Restart(int64_t offset, int64_t reference);

  // Takes |other|'s program and streams, for reading from the middle of
  // the input |other| probed (where the PAT and PMT may not repeat for a
  // while).
  void AdoptProgram(const TsReader& other);

  // Puts the timestamps of every run of |timeline| (which outlives the
  // reader) on one timeline: a stream whose timestamps jump back, or
  // ahead by more than a packet can last, goes on in the next run, which
  // is added where |timeline| does not know it yet. |reference| in
  // Restart() is then on that timeline too. Without it (HLS, which joins
  // segments itself), timestamps only unwrap.
  void JoinRuns(TsTimeline* timeline) { timeline_ = timeline; }
  // The run that |offset| is in.
  static size_t RunAt(const TsTimeline& timeline, int64_t offset);

  const std::vector<TsStream>& streams() const { return streams_; }
  // Where the first packet starts, and the packet size (188 or 192).
  int64_t data_start() const { return data_start_; }
  int packet_size() const { return packet_size_; }
  std::string error() const { return error_; }

  // 90 kHz ticks to microseconds and back.
  static int64_t TicksToMicros(int64_t ticks) { return ticks * 100 / 9; }
  static int64_t MicrosToTicks(int64_t micros) { return micros * 9 / 100; }
  // |ticks| (33 bits) as the value closest to |reference| (any range).
  static int64_t Nearest(int64_t ticks, int64_t reference);

 private:
  struct Pending {
    EncodedPacket packet;
    int64_t offset = 0;
    bool valid = false;
  };
  struct StreamState {
    // A packet waiting for the next one, which gives its duration.
    Pending pending;
    int64_t last_duration = 0;
    // AAC: bytes of an ADTS frame split across PES packets.
    std::vector<uint8_t> adts;
    int64_t adts_offset = 0;
    // The timestamp of the frame that starts adts_frames frames after
    // adts_anchor.
    int64_t adts_anchor = kNoTimestamp;
    int64_t adts_frames = 0;
    // With a timeline: the run the stream is in (-1: not known since the
    // restart), and its last DTS there in ticks.
    int run = -1;
    int64_t last_ticks = kNoTimestamp;
  };
  struct Queued {
    EncodedPacket packet;
    int64_t offset = 0;
  };

  // TsParser::Client
  void OnProgram(const std::vector<TsStreamEntry>& entries) override;
  void OnPes(const PesPacket& pes) override;

  // Finds the packet size and first sync byte at |begin_|.
  bool Sync(std::string* error);
  // Reads and parses one chunk. False on read errors; at the end of the
  // range sets eof_ and flushes.
  bool ReadMore();
  void Flush();
  void SplitAdts(int stream, const PesPacket& pes, int64_t pts);
  // Queues |packet| behind the previous one of its stream.
  void Emit(int stream, EncodedPacket packet, int64_t offset);
  int64_t Unwrap(int64_t ticks);
  // |pes|'s timestamps on |timeline_|.
  void Join(int stream, const PesPacket& pes, int64_t* pts, int64_t* dts);
  // The run a stream goes on in after a jump at |offset| to |ticks|.
  int NextRun(int run, int64_t offset, int64_t ticks);
  // Whether |stream|'s frames decide where runs join.
  bool IsAnchor(int stream) const;

  DataSource* const source_;
  TsParser parser_;
  int64_t begin_ = 0;
  int64_t end_ = -1;
  int64_t data_start_ = -1;
  int packet_size_ = static_cast<int>(kTsPacketSize);
  // Bytes read but not parsed, from buffer_offset_ on.
  std::vector<uint8_t> buffer_;
  int64_t buffer_offset_ = 0;
  int64_t read_position_ = 0;
  bool eof_ = false;
  bool flushed_ = false;
  bool synced_ = true;

  std::vector<TsStream> streams_;
  std::vector<StreamState> states_;
  std::map<int, int> stream_by_pid_;
  std::deque<Queued> queue_;
  int64_t reference_ = kNoTimestamp;
  int64_t last_ticks_ = kNoTimestamp;
  TsTimeline* timeline_ = nullptr;
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP2T_TS_READER_H_
