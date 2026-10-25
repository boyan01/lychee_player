#define LOG_TAG "subtitle_renderer"

#include "pipeline/subtitle_renderer.h"

#include <algorithm>

#include "base/log.h"
#include "media/factory.h"

namespace lychee {

SubtitleRenderer::SubtitleRenderer() = default;
SubtitleRenderer::~SubtitleRenderer() = default;

bool SubtitleRenderer::Initialize(const SubtitleConfig& config,
                                  std::string* error) {
  decoder_ = CreateSubtitleDecoder();
  if (!decoder_) {
    *error = "no subtitle decoder in this build";
    return false;
  }
  if (!decoder_->Initialize(config, error)) {
    decoder_.reset();
    return false;
  }
  return true;
}

void SubtitleRenderer::Decode(const EncodedPacket& packet) {
  if (!decoder_ || failed_) {
    return;
  }
  std::vector<SubtitleCue> cues;
  if (!decoder_->Decode(packet, &cues)) {
    LOGE("%s decoder failed; subtitles stop", decoder_->name());
    failed_ = true;
  }
  for (SubtitleCue& cue : cues) {
    AddCue(std::move(cue));
  }
}

void SubtitleRenderer::Flush() {
  if (decoder_) {
    decoder_->Flush();
  }
}

void SubtitleRenderer::AddCue(SubtitleCue cue) {
  if (cue.start == kNoTimestamp) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::upper_bound(
      cues_.begin(), cues_.end(), cue.start,
      [](int64_t start, const SubtitleCue& c) { return start < c.start; });
  // Read again after a seek.
  for (auto same = it; same != cues_.begin() && (same - 1)->start == cue.start;
       --same) {
    if ((same - 1)->text == cue.text) {
      return;
    }
  }
  cues_.insert(it, std::move(cue));
}

std::string SubtitleRenderer::TextAt(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string text;
  auto end = std::upper_bound(
      cues_.begin(), cues_.end(), media_time,
      [](int64_t time, const SubtitleCue& c) { return time < c.start; });
  for (auto it = cues_.begin(); it != end; ++it) {
    if (it->text.empty()) {
      continue;
    }
    int64_t cue_end = it->end;
    if (cue_end == kNoTimestamp) {
      // Open-ended: until the next cue that starts later.
      auto next = std::upper_bound(
          it, cues_.end(), it->start,
          [](int64_t start, const SubtitleCue& c) { return start < c.start; });
      cue_end = next != cues_.end() ? next->start : kInfiniteTime;
    }
    if (media_time >= cue_end) {
      continue;
    }
    if (!text.empty()) {
      text += '\n';
    }
    text += it->text;
  }
  return text;
}

}  // namespace lychee
