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
  decoder_ = CreateSubtitleDecoder(config);
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
  // The open-ended cues of the latest start before it now end at it.
  size_t changed = static_cast<size_t>(
      std::lower_bound(
          cues_.begin(), cues_.end(), start,
          [](const SubtitleCue& c, int64_t time) { return c.start < time; }) -
      cues_.begin());
  if (changed > 0) {
    const int64_t previous = cues_[changed - 1].start;
    changed = static_cast<size_t>(
        std::lower_bound(
            cues_.begin(), cues_.end(), previous,
            [](const SubtitleCue& c, int64_t time) { return c.start < time; }) -
        cues_.begin());
  }
  index_from_ = std::min(index_from_, changed);
  cues_.insert(it, std::move(cue));
  if (bitmap_bytes_ > kMaxBitmapBytes) {
    LimitBitmapsLocked(start);
  }
}

void SubtitleRenderer::DropEndedBy(int64_t time) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpdateIndexLocked();
  // Every cue before this one has ended by |time|.
  const auto ended =
      std::upper_bound(max_ends_.begin(), max_ends_.end(), time) -
      max_ends_.begin();
  if (ended == 0) {
    return;
  }
  for (auto it = cues_.begin(); it != cues_.begin() + ended; ++it) {
    bitmap_bytes_ -= BitmapBytes(*it);
  }
  cues_.erase(cues_.begin(), cues_.begin() + ended);
  // A cue's end depends only on the cues after it, and the latest ends
  // left are past |time|, so none of them was a dropped cue's.
  ends_.erase(ends_.begin(), ends_.begin() + ended);
  max_ends_.erase(max_ends_.begin(), max_ends_.begin() + ended);
  index_from_ = cues_.size();
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
      // Only the latest start's are left. They show together, but input
      // may give picture after picture for one time: the ones before the
      // newest (added last) go too.
      auto newest = cues_.end();
      for (auto it = cues_.begin(); it != cues_.end(); ++it) {
        if (it->bitmaps && it->start == latest_start) {
          furthest = furthest == cues_.end() ? it : furthest;
          newest = it;
        }
      }
      if (furthest == newest) {
        return;
      }
    }
    bitmap_bytes_ -= BitmapBytes(*furthest);
    // An empty cue in its place still ends the open-ended one before it.
    furthest->bitmaps.reset();
  }
}

void SubtitleRenderer::UpdateIndexLocked() const {
  const size_t count = cues_.size();
  if (index_from_ >= count && ends_.size() == count) {
    return;
  }
  ends_.resize(count);
  max_ends_.resize(count);
  // Open-ended: until the next cue that starts later.
  int64_t next_start = kInfiniteTime;
  for (size_t i = count; i-- > index_from_;) {
    if (i + 1 < count && cues_[i + 1].start != cues_[i].start) {
      next_start = cues_[i + 1].start;
    }
    ends_[i] = cues_[i].end != kNoTimestamp ? cues_[i].end : next_start;
  }
  for (size_t i = index_from_; i < count; ++i) {
    const SubtitleCue& cue = cues_[i];
    // A cue that shows nothing only ends the open-ended ones before it.
    // One emptied later (LimitBitmapsLocked()) still counts, which only
    // makes lookups start earlier.
    const int64_t end =
        cue.text.empty() && !cue.bitmaps ? kNoTimestamp : ends_[i];
    max_ends_[i] = i > 0 ? std::max(max_ends_[i - 1], end) : end;
  }
  index_from_ = count;
}

std::vector<const SubtitleCue*> SubtitleRenderer::ShowingLocked(
    int64_t media_time) const {
  UpdateIndexLocked();
  std::vector<const SubtitleCue*> showing;
  const size_t end = static_cast<size_t>(
      std::upper_bound(
          cues_.begin(), cues_.end(), media_time,
          [](int64_t time, const SubtitleCue& c) { return time < c.start; }) -
      cues_.begin());
  // Every cue before this one has ended by |media_time|.
  const size_t first = static_cast<size_t>(
      std::upper_bound(max_ends_.begin(),
                       max_ends_.begin() + static_cast<ptrdiff_t>(end),
                       media_time) -
      max_ends_.begin());
  for (size_t i = first; i < end; ++i) {
    const SubtitleCue& cue = cues_[i];
    if (cue.text.empty() && !cue.bitmaps) {
      continue;
    }
    if (media_time < ends_[i]) {
      showing.push_back(&cue);
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
