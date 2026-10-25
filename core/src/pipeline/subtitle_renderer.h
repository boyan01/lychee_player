#ifndef LYCHEE_PIPELINE_SUBTITLE_RENDERER_H_
#define LYCHEE_PIPELINE_SUBTITLE_RENDERER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/subtitle_decoder.h"

namespace lychee {

// Decodes a text subtitle stream into cues and answers which text is on
// screen at a media time. Decoding is cheap, so it runs on the read thread as
// packets arrive. Cues are kept across seeks, since a seek may land after
// the packet of a cue still on screen; cues read again are ignored.
class SubtitleRenderer {
 public:
  SubtitleRenderer();
  ~SubtitleRenderer();

  // Without a decoder, cues can only be added with AddCue().
  bool Initialize(const SubtitleConfig& config, std::string* error);

  // Read thread.
  void Decode(const EncodedPacket& packet);
  // After a seek.
  void Flush();

  void AddCue(SubtitleCue cue);
  // Text of the cues showing at |media_time|, in start order, one per line;
  // empty if none.
  std::string TextAt(int64_t media_time) const;

 private:
  std::unique_ptr<SubtitleDecoder> decoder_;
  bool failed_ = false;

  mutable std::mutex mutex_;
  // Sorted by start time.
  std::vector<SubtitleCue> cues_;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_SUBTITLE_RENDERER_H_
