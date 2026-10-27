#ifndef LYCHEE_PIPELINE_SUBTITLE_RENDERER_H_
#define LYCHEE_PIPELINE_SUBTITLE_RENDERER_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/subtitle_decoder.h"

namespace lychee {

// A subtitle picture laid out on the canvas the caller draws the video on.
struct SubtitleImage {
  // Where it goes on the canvas.
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  // The pixels, drawn scaled to the rectangle above.
  const SubtitleBitmap* bitmap = nullptr;
};

// What lychee_player_acquire_subtitle() hands out. Holds the bitmaps it
// points to.
struct SubtitleImages {
  // Differs between sets with different pictures.
  uint64_t id = 0;
  std::vector<SubtitleImage> images;
  std::vector<std::shared_ptr<const std::vector<SubtitleBitmap>>> bitmaps;
};

// Decodes a subtitle stream into cues and answers what is on screen at a
// media time: text, and pictures for bitmap subtitles. Decoding is cheap,
// so it runs on the read thread as packets arrive. Cues are kept across seeks,
// since a seek may land after the packet of a cue still on screen; cues read
// again are ignored. Bitmap cues are dropped, the furthest from the latest
// first, once they hold more than kMaxBitmapBytes.
class SubtitleRenderer {
 public:
  static constexpr size_t kMaxBitmapBytes = 32 << 20;

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
  // Identifies the pictures showing at |media_time|: 0 if none.
  uint64_t ImagesKeyAt(int64_t media_time) const;
  // The pictures at |media_time| on a |width| x |height| canvas over a
  // video of |video_width| x |video_height|; null if none. The same object
  // as the last call while they do not change.
  std::shared_ptr<const SubtitleImages> ImagesAt(int64_t media_time,
                                                 int width,
                                                 int height,
                                                 int video_width,
                                                 int video_height);

 private:
  // The cues showing at |media_time|, in start order.
  std::vector<const SubtitleCue*> ShowingLocked(int64_t media_time) const;
  void LimitBitmapsLocked(int64_t latest_start);

  std::unique_ptr<SubtitleDecoder> decoder_;
  bool failed_ = false;

  mutable std::mutex mutex_;
  // Sorted by start time.
  std::vector<SubtitleCue> cues_;
  size_t bitmap_bytes_ = 0;
  // The last ImagesAt() result and what it was made for.
  std::shared_ptr<const SubtitleImages> images_;
  uint64_t images_key_ = 0;
  int images_width_ = 0;
  int images_height_ = 0;
};

}  // namespace lychee

#endif  // LYCHEE_PIPELINE_SUBTITLE_RENDERER_H_
