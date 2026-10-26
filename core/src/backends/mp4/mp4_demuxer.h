#ifndef LYCHEE_BACKENDS_MP4_MP4_DEMUXER_H_
#define LYCHEE_BACKENDS_MP4_MP4_DEMUXER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// MP4 and QuickTime files (ISO/IEC 14496-12), for builds without FFmpeg:
// sample tables in the moov box, fragmented MP4 (moof boxes after a moov
// with mvex), or both. Video, audio and tx3g (mov_text) subtitle tracks;
// codec configuration is passed through as the container stores it
// (avcC, hvcC, the AudioSpecificConfig, ALAC's cookie, the tx3g entry).
//
// Fragmented files are read one moof at a time. Seeks find the fragment
// in mfra or a sidx that covers the file, or else by reading the moof
// boxes from the last one known (on http, a range request each).
class Mp4Demuxer : public Demuxer {
 public:
  Mp4Demuxer();
  ~Mp4Demuxer() override;

  const char* name() const override { return "mp4"; }
  bool Open(const std::string& url, std::string* error) override;
  // Reads |source| instead of a URL (tests, the fuzzer).
  bool Open(std::unique_ptr<DataSource> source, std::string* error);
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }

 private:
  struct Sample {
    uint64_t offset;
    uint32_t size;
    bool sync;
    // Microseconds on the presentation timeline (edit list applied).
    int64_t dts;
    int64_t pts;
    int64_t duration;
  };
  struct Track {
    // All of the track's samples, or in fragmented files those of the
    // fragments read since the last one was consumed.
    std::vector<Sample> samples;
    size_t next = 0;
    bool enabled = true;
    MediaType type = MediaType::kUnknown;
    // From tkhd, mdhd and the edit list: media time |media_time| (in
    // |timescale| units) is at |empty| microseconds.
    uint32_t id = 0;
    int64_t timescale = 1;
    int64_t media_time = 0;
    int64_t empty = 0;
    // Decode times are this much earlier than stored, so that negative
    // composition offsets (ctts or trun version 1) leave no sample with
    // pts < dts, which the decoders' reordering relies on (as FFmpeg's
    // dts_shift).
    int64_t dts_shift = 0;
    // Fragmented files: the trex defaults, the moov's own samples (read
    // before the first moof) and the decode time after them, the decode
    // time after the last fragment read (for a traf without tfdt), and
    // after a seek, samples that end before this time are skipped.
    uint32_t default_duration = 0;
    uint32_t default_size = 0;
    uint32_t default_flags = 0;
    std::vector<Sample> moov_samples;
    int64_t moov_decode_time = 0;
    int64_t decode_time = 0;
    int64_t skip_before = kNoTimestamp;
  };
  // A place to start reading a fragmented file: the moof at |offset| (or
  // the moov's samples, kMoovSamples) holds a sync sample at about |time|.
  struct SeekPoint {
    int64_t time;
    int64_t offset;
  };
  // What a moof holds: its earliest decode time, the first sync sample of
  // the seek track, and where its samples end.
  struct MoofSummary {
    int64_t start = kNoTimestamp;
    int64_t sync_time = kNoTimestamp;
    int64_t end = kNoTimestamp;
  };
  enum class LoadResult { kLoaded, kEnd, kError, kAborted };

  // |data| is a moov box's payload.
  bool ParseMoov(const uint8_t* data, size_t size, std::string* error);
  bool ParseTrack(const uint8_t* data, size_t size, int64_t movie_timescale);
  void ParseMetadata(const uint8_t* data, size_t size);
  Sample MakeSample(const Track& track,
                    uint64_t offset,
                    uint32_t size,
                    bool sync,
                    int64_t dts,
                    int64_t composition,
                    int64_t duration) const;

  // Fragmented MP4. A moof box's bytes, from its header; appends its
  // samples to the enabled tracks if |store|, and reads and advances
  // |decode_times| (one per track) where a traf has no tfdt.
  void ParseMoof(const uint8_t* data,
                 size_t size,
                 int64_t offset,
                 bool store,
                 std::vector<int64_t>* decode_times,
                 MoofSummary* summary);
  // Reads the top-level box at |offset|. kEnd at the end of the data.
  LoadResult ReadBoxHeader(int64_t offset, uint32_t* type, uint64_t* size,
                           uint64_t* header);
  // Reads the moof at |offset| into |moof|, setting error_ on failure.
  LoadResult ReadMoof(int64_t offset, uint64_t size, std::vector<uint8_t>* moof);
  // Reads the next moof into the tracks.
  LoadResult LoadNextFragment();
  // Loads fragments until every enabled track has a sample to read, or
  // none can come before the ones loaded.
  LoadResult FillFragments();
  // Each track's decode time at |time|, for fragments without tfdt.
  std::vector<int64_t> DecodeTimesAt(int64_t time) const;
  // Starts reading over at a seek point.
  LoadResult StartAt(int64_t offset, int64_t time);
  // Sets up a fragmented file once the moov is read: index, first
  // fragment, duration.
  bool OpenFragments(std::string* error);
  bool ReadMfra();
  void ReadSidx();
  // Reads the moof boxes from |*offset| on, advancing it and
  // |decode_times| and raising fragment_end_, and adds seek points if
  // |add_points|. kLoaded once a seek point is past |until|, kEnd at the
  // end of the data.
  LoadResult Scan(int64_t* offset,
                  std::vector<int64_t>* decode_times,
                  int64_t until,
                  bool add_points);
  // Seek points up to past |time|, from scan_offset_ on; the index is
  // complete once the scan reaches the end.
  LoadResult ScanIndex(int64_t time);
  bool SeekFragments(int64_t time);
  // |size| bytes at |offset|: kLoaded, kEnd if the data ends first.
  LoadResult ReadFully(int64_t offset, uint8_t* data, size_t size);
  DemuxerReadResult Fail(int64_t at, const char* reason, const char* detail);
  bool aborted() const { return aborted_ || interrupted_; }

  // Set once by Open(); the mutex is for Interrupt() and Abort() on other
  // threads meanwhile.
  std::mutex source_mutex_;
  std::unique_ptr<DataSource> source_;
  MediaInfo media_info_;
  // Parallel to media_info_.streams.
  std::vector<Track> tracks_;
  // While parsing: the file's size (-1 if unknown), and how many samples
  // the tracks may still have (kMaxSamples in all).
  int64_t file_size_ = -1;
  size_t sample_budget_ = 0;

  // Fragmented MP4: a moov with mvex.
  bool fragmented_ = false;
  // The mehd box's duration, kNoTimestamp without one.
  int64_t fragment_duration_ = kNoTimestamp;
  // The first top-level box after the moov.
  int64_t first_fragment_offset_ = 0;
  // The next top-level box to read; reading is at the end once a box is
  // not there.
  int64_t next_fragment_offset_ = 0;
  bool fragments_ended_ = false;
  // The earliest decode time in the last moof read.
  int64_t last_fragment_start_ = kNoTimestamp;
  // Index into tracks_ of the track seek points are for: the default
  // video track, or the first.
  int seek_track_ = 0;
  // Sorted by time. Complete from mfra or a sidx; otherwise it covers the
  // moof boxes before |scan_offset_|, which are read on demand.
  std::vector<SeekPoint> seek_points_;
  bool index_complete_ = false;
  int64_t scan_offset_ = 0;
  std::vector<int64_t> scan_decode_times_;
  // The latest sample end seen while scanning.
  int64_t fragment_end_ = kNoTimestamp;

  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP4_MP4_DEMUXER_H_
