#define LOG_TAG "subtitle_renderer"

#include "pipeline/subtitle_renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "base/log.h"
#include "media/factory.h"

namespace lychee {

namespace {

std::atomic<uint64_t> next_images_id{1};

size_t BitmapBytes(const SubtitleCue& cue) {
  size_t bytes = 0;
  if (cue.bitmaps) {
    for (const SubtitleBitmap& bitmap : *cue.bitmaps) {
      bytes += bitmap.rgba.size();
    }
  }
  return bytes;
}

}  // namespace

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
    const SubtitleCue& previous = *(same - 1);
    if (previous.end == cue.end && previous.text == cue.text &&
        previous.canvas_width == cue.canvas_width &&
        previous.canvas_height == cue.canvas_height &&
        (previous.bitmaps == cue.bitmaps ||
         (previous.bitmaps && cue.bitmaps &&
          *previous.bitmaps == *cue.bitmaps))) {
      return;
    }
  }
  const int64_t start = cue.start;
  bitmap_bytes_ += BitmapBytes(cue);
  cues_.insert(it, std::move(cue));
  if (bitmap_bytes_ > kMaxBitmapBytes) {
    LimitBitmapsLocked(start);
  }
}

void SubtitleRenderer::LimitBitmapsLocked(int64_t latest_start) {
  while (bitmap_bytes_ > kMaxBitmapBytes) {
    auto furthest = cues_.end();
    for (auto it = cues_.begin(); it != cues_.end(); ++it) {
      if (it->bitmaps && it->start != latest_start &&
          (furthest == cues_.end() ||
           std::llabs(it->start - latest_start) >
               std::llabs(furthest->start - latest_start))) {
        furthest = it;
      }
    }
    if (furthest == cues_.end()) {
      return;
    }
    bitmap_bytes_ -= BitmapBytes(*furthest);
    // An empty cue in its place still ends the open-ended one before it.
    furthest->bitmaps.reset();
  }
}

std::vector<const SubtitleCue*> SubtitleRenderer::ShowingLocked(
    int64_t media_time) const {
  std::vector<const SubtitleCue*> showing;
  auto end = std::upper_bound(
      cues_.begin(), cues_.end(), media_time,
      [](int64_t time, const SubtitleCue& c) { return time < c.start; });
  for (auto it = cues_.begin(); it != end; ++it) {
    if (it->text.empty() && !it->bitmaps) {
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
    if (media_time < cue_end) {
      showing.push_back(&*it);
    }
  }
  return showing;
}

std::string SubtitleRenderer::TextAt(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string text;
  for (const SubtitleCue* cue : ShowingLocked(media_time)) {
    if (cue->text.empty()) {
      continue;
    }
    if (!text.empty()) {
      text += '\n';
    }
    text += cue->text;
  }
  return text;
}

uint64_t SubtitleRenderer::ImagesKeyAt(int64_t media_time) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // FNV-1a over the start times of the pictures showing.
  uint64_t key = 0;
  for (const SubtitleCue* cue : ShowingLocked(media_time)) {
    if (cue->bitmaps && !cue->bitmaps->empty()) {
      key = (key ^ static_cast<uint64_t>(cue->start)) * 1099511628211u;
      key |= 1;
    }
  }
  return key;
}

std::shared_ptr<const SubtitleImages> SubtitleRenderer::ImagesAt(
    int64_t media_time,
    int width,
    int height,
    int video_width,
    int video_height) {
  if (width <= 0 || height <= 0) {
    width = video_width;
    height = video_height;
  }
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  const uint64_t key = ImagesKeyAt(media_time);
  std::lock_guard<std::mutex> lock(mutex_);
  if (key == images_key_ && width == images_width_ &&
      height == images_height_) {
    return images_;
  }
  images_key_ = key;
  images_width_ = width;
  images_height_ = height;
  images_ = nullptr;
  if (key == 0) {
    return nullptr;
  }
  auto images = std::make_shared<SubtitleImages>();
  images->id = next_images_id++;
  for (const SubtitleCue* cue : ShowingLocked(media_time)) {
    if (!cue->bitmaps) {
      continue;
    }
    // From the subtitle's canvas to the caller's.
    const double canvas_width =
        cue->canvas_width > 0 ? cue->canvas_width : video_width;
    const double canvas_height =
        cue->canvas_height > 0 ? cue->canvas_height : video_height;
    if (canvas_width <= 0 || canvas_height <= 0) {
      continue;
    }
    const double sx = width / canvas_width;
    const double sy = height / canvas_height;
    for (const SubtitleBitmap& bitmap : *cue->bitmaps) {
      SubtitleImage image;
      image.x = static_cast<int>(std::lround(bitmap.x * sx));
      image.y = static_cast<int>(std::lround(bitmap.y * sy));
      image.width =
          std::max(static_cast<int>(std::lround(bitmap.width * sx)), 1);
      image.height =
          std::max(static_cast<int>(std::lround(bitmap.height * sy)), 1);
      image.bitmap = &bitmap;
      images->images.push_back(image);
    }
    images->bitmaps.push_back(cue->bitmaps);
  }
  images_ = std::move(images);
  return images_;
}

}  // namespace lychee
