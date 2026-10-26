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

// MP4 and QuickTime files (ISO/IEC 14496-12) with their sample tables in
// the moov box, for builds without FFmpeg. Fragmented MP4 is not read.
// Video and audio tracks only; codec configuration is passed through as
// the container stores it (avcC, hvcC, the AudioSpecificConfig, ALAC's
// cookie).
class Mp4Demuxer : public Demuxer {
 public:
  Mp4Demuxer();
  ~Mp4Demuxer() override;

  const char* name() const override { return "mp4"; }
  bool Open(const std::string& url, std::string* error) override;
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
    std::vector<Sample> samples;
    size_t next = 0;
    bool enabled = true;
  };

  // |data| is a moov box's payload.
  bool ParseMoov(const uint8_t* data, size_t size, std::string* error);
  bool ParseTrack(const uint8_t* data, size_t size, int64_t movie_timescale);
  void ParseMetadata(const uint8_t* data, size_t size);

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
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_MP4_MP4_DEMUXER_H_
