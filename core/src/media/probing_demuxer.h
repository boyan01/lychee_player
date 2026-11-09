#ifndef LYCHEE_MEDIA_PROBING_DEMUXER_H_
#define LYCHEE_MEDIA_PROBING_DEMUXER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/data_source.h"
#include "media/demuxer.h"

namespace lychee {

// Builds without FFmpeg have one demuxer per container. This one opens
// the input, looks at its first bytes, and hands it to the demuxer for
// what it finds: DASH, HLS, MPEG-TS, Matroska/WebM, standalone audio, FLV, AVI,
// MPEG-PS, Windows ASF, and MP4 for anything else. Unknown input fails
// with MP4's error.
//
// With a fallback (FFmpeg's demuxer), DASH, HLS, MPEG-TS, AVI, MPEG-PS and
// Windows ASF use native readers. HLS and MPEG-TS join timestamps that start
// over where FFmpeg does not; ASF uses Windows' own reader and decoder MFTs.
// The fallback takes everything else, input that OpenDataSource cannot
// read, and what the own demuxers fail to open or would play only in
// part (TsDemuxer::set_refuse_skipped_streams(),
// HlsDemuxer::set_refuse_subtitles()).
class ProbingDemuxer : public Demuxer {
 public:
  // An unopened demuxer for the input; |probed| is its source with the
  // first bytes read (null: open the URL anew).
  using Fallback = std::function<std::unique_ptr<Demuxer>(
      std::unique_ptr<DataSource> probed)>;

  // Every http(s) request of the input carries |headers|
  // (NormalizeHttpHeaders()); the fallback is the caller's to give them.
  explicit ProbingDemuxer(Fallback fallback = nullptr,
                          std::string headers = {});
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
  int64_t download_rate() const override;
  std::vector<VariantInfo> variants() const override;
  int current_variant() const override;
  void SetVariantPolicy(int pinned, int64_t max_bandwidth) override;
  bool live_window(LiveWindow* window) const override;

 private:
  // Makes |demuxer| the one calls go to; it may then open.
  void Adopt(std::unique_ptr<Demuxer> demuxer);
  // Open(), with the demuxers' errors as they give them.
  bool OpenInput(const std::string& url, std::string* error);
  // Opens |url| with the fallback, unless aborted. |why| is what the own
  // demuxers made of it, logged.
  bool OpenFallback(std::unique_ptr<DataSource> probed,
                    const std::string& url,
                    const std::string& why,
                    std::string* error);

  const Fallback fallback_;
  const std::string headers_;
  mutable std::mutex mutex_;
  // The source read for the first bytes, while it is.
  DataSource* probing_ = nullptr;
  std::unique_ptr<Demuxer> demuxer_;
  std::atomic<bool> aborted_{false};
  // SetVariantPolicy(), for the demuxer adopted.
  int pinned_ = -1;
  int64_t max_bandwidth_ = 0;
  MediaInfo empty_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_PROBING_DEMUXER_H_
