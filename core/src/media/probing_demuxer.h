#ifndef LYCHEE_MEDIA_PROBING_DEMUXER_H_
#define LYCHEE_MEDIA_PROBING_DEMUXER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// Builds without FFmpeg have one demuxer per container. This one opens
// the input, looks at its first bytes, and hands it to the demuxer for
// what it finds: an HLS playlist, MPEG-TS, Matroska/WebM, and MP4 for
// anything else, so unknown input fails with MP4's error.
class ProbingDemuxer : public Demuxer {
 public:
  ProbingDemuxer();
  ~ProbingDemuxer() override;

  const char* name() const override;
  bool Open(const std::string& url, std::string* error) override;
  const MediaInfo& media_info() const override;
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override;

 private:
  // Makes |demuxer| the one calls go to; it may then open.
  void Adopt(std::unique_ptr<Demuxer> demuxer);

  std::mutex mutex_;
  // The source read for the first bytes, while it is.
  DataSource* probing_ = nullptr;
  std::unique_ptr<Demuxer> demuxer_;
  std::atomic<bool> aborted_{false};
  MediaInfo empty_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_PROBING_DEMUXER_H_
