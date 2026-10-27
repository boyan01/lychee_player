#define LOG_TAG "libass"

#include "backends/libass/libass_rasterizer.h"

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdio>

#include "base/log.h"

#if defined(LYCHEE_LIBASS_DEFAULT_FAMILY)
#include <unistd.h>

// default_font.S
extern "C" const char lychee_default_font_regular[];
extern "C" const char lychee_default_font_regular_end[];
extern "C" const char lychee_default_font_bold[];
extern "C" const char lychee_default_font_bold_end[];
#endif

namespace lychee {

namespace {

void OnMessage(int level, const char* format, va_list args, void*) {
  // 0 fatal ... 7 debug; 2 and below are errors.
  if (level > 2) {
    return;
  }
  char message[512];
  vsnprintf(message, sizeof(message), format, args);
  LOGW("%s", message);
}

}  // namespace

LibassRasterizer::LibassRasterizer() = default;

LibassRasterizer::~LibassRasterizer() {
  if (track_) {
    ass_free_track(track_);
  }
  if (renderer_) {
    ass_renderer_done(renderer_);
  }
  if (library_) {
    ass_library_done(library_);
  }
}

bool LibassRasterizer::Initialize(const SubtitleConfig& config,
                                  const std::vector<SubtitleFont>& fonts,
                                  std::string* error) {
  library_ = ass_library_init();
  if (!library_) {
    *error = "ass_library_init failed";
    return false;
  }
  ass_set_message_cb(library_, &OnMessage, nullptr);
  for (const SubtitleFont& font : fonts) {
    ass_add_font(library_, font.name.c_str(),
                 reinterpret_cast<const char*>(font.data->data()),
                 static_cast<int>(font.data->size()));
  }
#if defined(LYCHEE_LIBASS_DEFAULT_FAMILY)
  // Bold too: libass's synthetic bold is much lighter than a bold face.
  // Italics are slanted well enough.
  ass_add_font(library_, "default", lychee_default_font_regular,
               static_cast<int>(lychee_default_font_regular_end -
                                lychee_default_font_regular));
  ass_add_font(library_, "default-bold", lychee_default_font_bold,
               static_cast<int>(lychee_default_font_bold_end -
                                lychee_default_font_bold));
#endif
  renderer_ = ass_renderer_init(library_);
  track_ = ass_new_track(library_);
  if (!renderer_ || !track_) {
    *error = "ass_renderer_init failed";
    return false;
  }
#if defined(LYCHEE_LIBASS_DEFAULT_FAMILY)
  // No font provider (Android): fonts the media does not carry are the
  // default font, metrically compatible with Arial, which most scripts
  // name. Glyphs it lacks (CJK) come from the system's CJK font, which
  // libass reads from its file, like the fonts it would find through a
  // provider.
  const char* fallback = "/system/fonts/NotoSansCJK-Regular.ttc";
  ass_set_fonts(renderer_, access(fallback, R_OK) == 0 ? fallback : nullptr,
                LYCHEE_LIBASS_DEFAULT_FAMILY, ASS_FONTPROVIDER_NONE, nullptr,
                0);
#else
  // The system's fonts (CoreText, DirectWrite or fontconfig) for what the
  // media does not carry. Fontconfig may build its cache here once.
  ass_set_fonts(renderer_, nullptr, "sans-serif", ASS_FONTPROVIDER_AUTODETECT,
                nullptr, 1);
#endif
  if (!config.extradata.empty()) {
    ass_process_codec_private(
        track_,
        const_cast<char*>(reinterpret_cast<const char*>(config.extradata.data())),
        static_cast<int>(config.extradata.size()));
  }
  LOGI("styled subtitles through libass %x, %zu embedded fonts",
       ass_library_version(), fonts.size());
  return true;
}

void LibassRasterizer::Decode(const EncodedPacket& packet) {
  if (packet.end_of_stream || packet.pts == kNoTimestamp ||
      packet.size() == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Read again after a seek: libass drops events whose ReadOrder it has.
  ass_process_chunk(
      track_,
      const_cast<char*>(reinterpret_cast<const char*>(packet.buffer->data())),
      static_cast<int>(packet.size()), packet.pts / kMicrosPerMilli,
      std::max<int64_t>(packet.duration, 0) / kMicrosPerMilli);
}

uint64_t LibassRasterizer::EventsAt(int64_t media_time) {
  const long long ms = media_time / kMicrosPerMilli;
  std::lock_guard<std::mutex> lock(mutex_);
  // FNV-1a over the events showing.
  uint64_t key = 0;
  for (int i = 0; i < track_->n_events; ++i) {
    const ASS_Event& event = track_->events[i];
    if (event.Start <= ms && ms < event.Start + event.Duration) {
      key = ((key ^ static_cast<uint64_t>(i)) * 1099511628211u) | 1;
    }
  }
  return key;
}

bool LibassRasterizer::Render(int64_t media_time,
                              int width,
                              int height,
                              int video_width,
                              int video_height,
                              std::vector<SubtitleBitmap>* bitmaps) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool resized = !rendered_;
  if (width != width_ || height != height_) {
    width_ = width;
    height_ = height;
    ass_set_frame_size(renderer_, width, height);
    resized = true;
  }
  if (video_width > 0 && video_height > 0 &&
      (video_width != video_width_ || video_height != video_height_)) {
    video_width_ = video_width;
    video_height_ = video_height;
    ass_set_storage_size(renderer_, video_width, video_height);
    resized = true;
  }
  int change = 0;
  const ASS_Image* images = ass_render_frame(
      renderer_, track_, media_time / kMicrosPerMilli, &change);
  rendered_ = true;
  if (change == 0 && !resized) {
    return false;
  }
  int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
  for (const ASS_Image* image = images; image; image = image->next) {
    if (image->w <= 0 || image->h <= 0) {
      continue;
    }
    left = std::min(left, image->dst_x);
    top = std::min(top, image->dst_y);
    right = std::max(right, image->dst_x + image->w);
    bottom = std::max(bottom, image->dst_y + image->h);
  }
  if (left >= right || top >= bottom) {
    return true;
  }
  SubtitleBitmap bitmap;
  bitmap.x = left;
  bitmap.y = top;
  bitmap.width = right - left;
  bitmap.height = bottom - top;
  bitmap.rgba.assign(static_cast<size_t>(bitmap.width) * bitmap.height * 4, 0);
  // Glyphs, outlines and shadows come back to front: blend each over what
  // is there, in premultiplied alpha.
  for (const ASS_Image* image = images; image; image = image->next) {
    const uint32_t r = image->color >> 24;
    const uint32_t g = (image->color >> 16) & 0xff;
    const uint32_t b = (image->color >> 8) & 0xff;
    const uint32_t opacity = 255 - (image->color & 0xff);
    for (int y = 0; y < image->h; ++y) {
      const uint8_t* coverage = image->bitmap + static_cast<ptrdiff_t>(y) * image->stride;
      uint8_t* out = bitmap.rgba.data() +
                     (static_cast<size_t>(image->dst_y - top + y) * bitmap.width +
                      (image->dst_x - left)) * 4;
      for (int x = 0; x < image->w; ++x, out += 4) {
        const uint32_t k = (coverage[x] * opacity + 127) / 255;
        if (k == 0) {
          continue;
        }
        const uint32_t keep = 255 - k;
        out[0] = static_cast<uint8_t>((r * k + out[0] * keep + 127) / 255);
        out[1] = static_cast<uint8_t>((g * k + out[1] * keep + 127) / 255);
        out[2] = static_cast<uint8_t>((b * k + out[2] * keep + 127) / 255);
        out[3] = static_cast<uint8_t>((255 * k + out[3] * keep + 127) / 255);
      }
    }
  }
  bitmaps->push_back(std::move(bitmap));
  return true;
}

}  // namespace lychee
