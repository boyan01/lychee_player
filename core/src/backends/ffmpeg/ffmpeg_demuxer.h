#ifndef LYCHEE_BACKENDS_FFMPEG_FFMPEG_DEMUXER_H_
#define LYCHEE_BACKENDS_FFMPEG_FFMPEG_DEMUXER_H_

#include <atomic>
#include <string>
#include <vector>

#include "media/demuxer.h"

struct AVFormatContext;
struct AVPacket;

namespace lychee {

class FFmpegDemuxer : public Demuxer {
 public:
  FFmpegDemuxer();
  ~FFmpegDemuxer() override;

  const char* name() const override { return "ffmpeg"; }
  bool Open(const std::string& url, std::string* error) override;
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }

 private:
  static int InterruptCallback(void* opaque);
  void BuildMediaInfo();
  // At end of file: whether the data stopped short of the duration the
  // container declares.
  bool TruncatedAtEnd() const;

  AVFormatContext* format_context_ = nullptr;
  AVPacket* av_packet_ = nullptr;
  std::atomic<bool> aborted_{false};
  std::atomic<bool> interrupted_{false};
  MediaInfo media_info_;
  std::vector<bool> enabled_streams_;
  std::string error_;
  // The duration comes from the container's index rather than an estimate.
  bool declared_duration_ = false;
  // End of the data read since open or the last seek.
  int64_t read_end_ = kNoTimestamp;
};

}  // namespace lychee

#endif  // LYCHEE_BACKENDS_FFMPEG_FFMPEG_DEMUXER_H_
