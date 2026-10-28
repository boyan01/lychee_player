#ifndef LYCHEE_BACKENDS_MP2T_TS_DEMUXER_H_
#define LYCHEE_BACKENDS_MP2T_TS_DEMUXER_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "backends/mp2t/ts_reader.h"
#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// MPEG-TS files and streams (ISO/IEC 13818-1) for builds without FFmpeg:
// the first program's H.264, HEVC and AAC (ADTS) streams; MP3 and AC-3
// are listed but have no decoder there. Seeks by bisection on PES
// timestamps when the source can seek; the duration comes from the
// timestamps at the end of the input. Where timestamps start over
// inside the input, each run of them goes on where the one before ended
// (TsTimeline); the open samples timestamps across a seekable input to
// find those runs without reading all of it.
class TsDemuxer : public Demuxer {
 public:
  TsDemuxer();
  ~TsDemuxer() override;

  const char* name() const override { return "mpegts"; }
  bool Open(const std::string& url, std::string* error) override;
  // Takes a source already opened (and partly read) by the format probe.
  bool Open(std::unique_ptr<DataSource> source, std::string* error);
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }

  // Makes Open() fail early, before it reads on, where the program has
  // audio, video or subtitles it would skip: MPEG-2 video, DVB subtitles,
  // MPEG audio Layer II, ... (TsReader::skipped_stream_types()). For
  // builds with a demuxer that plays them (FFmpeg's). Set before Open().
  void set_refuse_skipped_streams(bool refuse) { refuse_skipped_ = refuse; }

 private:
  // The first PES packet of |pid| with a PTS at or after |offset| (within
  // a window), its PTS unwrapped next to |near|. False with *read_error
  // set if the source failed.
  bool PtsAt(int64_t offset,
             int pid,
             int64_t near,
             int64_t* pts,
             bool* read_error);
  // A packet boundary at or before the data whose |pid| timestamps reach
  // |time|, within the run that plays at |time|; -1 if the source failed.
  int64_t Bisect(int pid, int64_t time);
  void EstimateDuration();
  // Adds the runs that samples of |pid|'s timestamps across the input
  // show. False if the source failed.
  bool FindRuns(int pid);
  // Finds and adds the run that starts between |before| and |after|
  // (offset, PTS as read), where timestamps went back; then looks for
  // another one up to |after|.
  bool LocateRun(int pid,
                 std::pair<int64_t, int64_t> before,
                 std::pair<int64_t, int64_t> after);
  // A run starting at |offset| with |start|, the run before it having
  // ended at |end| (both as read).
  void AddRun(int64_t offset, int64_t start, int64_t end);
  bool Failed();

  std::mutex source_mutex_;
  std::unique_ptr<DataSource> source_;
  std::unique_ptr<TsReader> reader_;
  MediaInfo media_info_;
  TsTimeline timeline_;
  // PtsAt() results of the seek in progress: offset -> 33-bit PTS, or -1.
  std::map<int64_t, int64_t> pts_cache_;
  std::vector<bool> enabled_;
  // After an open or seek: video starts at a keyframe at or after this
  // offset, audio with the frame that plays at audio_from_.
  std::vector<bool> need_keyframe_;
  int64_t video_from_offset_ = -1;
  int64_t audio_from_ = kNoTimestamp;
  bool refuse_skipped_ = false;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP2T_TS_DEMUXER_H_
