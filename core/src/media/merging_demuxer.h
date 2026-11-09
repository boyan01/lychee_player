#ifndef LYCHEE_MEDIA_MERGING_DEMUXER_H_
#define LYCHEE_MEDIA_MERGING_DEMUXER_H_

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/demuxer.h"

namespace lychee {

// Plays a video input and separate audio inputs (DASH-style: a video-only
// file, an audio-only file per quality or language) as one input. Its
// streams are the main input's, then each audio input's audio streams;
// packets come in decode order across the inputs, seeks go to each, and
// Interrupt() and Abort() reach all, so the pipeline sees one demuxer.
// The inputs share a timeline (as DASH Representations do); the duration,
// the variants and the live window are the main input's. Only the inputs
// with an enabled stream are read.
class MergingDemuxer : public Demuxer {
 public:
  // |main| plays the URL given to Open(); |audio[i]| opens audio_urls[i]
  // (lychee_open_options.audio_urls).
  MergingDemuxer(std::unique_ptr<Demuxer> main,
                 std::vector<std::unique_ptr<Demuxer>> audio,
                 std::vector<std::string> audio_urls);
  ~MergingDemuxer() override;

  const char* name() const override;
  // Opens the inputs at once, each on a thread of its own but the main
  // one; fails if any fails or an audio input has no audio.
  bool Open(const std::string& url, std::string* error) override;
  const MediaInfo& media_info() const override { return media_info_; }
  void SetEnabledStreams(const std::vector<int>& stream_indices) override;
  DemuxerReadResult Read(EncodedPacket* packet) override;
  bool Seek(int64_t time) override;
  void Interrupt() override;
  void Abort() override;
  std::string error() const override { return error_; }
  int64_t download_rate() const override;
  std::vector<VariantInfo> variants() const override;
  int current_variant() const override;
  void SetVariantPolicy(int pinned, int64_t max_bandwidth) override;
  bool live_window(LiveWindow* window) const override;

 private:
  struct Input {
    std::unique_ptr<Demuxer> demuxer;
    // Its stream index -> media_info_'s, or -1 (an audio input's other
    // streams).
    std::vector<int> streams;
    bool enabled = false;
    bool ended = false;
    // The next packet, read ahead to interleave the inputs.
    bool has_next = false;
    EncodedPacket next;
  };

  // inputs_[0] is the main one.
  std::vector<Input> inputs_;
  const std::vector<std::string> audio_urls_;
  MediaInfo media_info_;
  std::string error_;
};

}  // namespace lychee

#endif  // LYCHEE_MEDIA_MERGING_DEMUXER_H_
