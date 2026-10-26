// Headless player: plays a file while pulling frames at a simulated vsync
// (60 Hz by default), and reports timing problems. Used for regression
// testing without a UI toolkit. With --virtual-time it runs on the library's
// test clock (core/src/api/testing.h): as fast as decoding allows, and
// independent of machine load. With --live it plays the file as a live
// input instead, pushing each packet when a sender would send it.

#include <lychee/lychee.h>

#include "testing.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#elif defined(_WIN32)
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace {

struct Options {
  double refresh_rate = 60;
  // Random error added to each vsync timestamp, in microseconds.
  int64_t vsync_jitter = 0;
  std::string url;
  double seconds = 0;
  double seek_to = -1;
  double seek_after = 2;
  float volume = 0;
  double rate = 1;
  // Seconds after playback starts to apply |rate|; < 0: before opening.
  double rate_after = -1;
  bool loop = false;
  // Turn looping off this many seconds after playback starts.
  double loop_off_after = -1;
  // Open |reopen_url| on the same player this many seconds after playback
  // starts; 0: right after the first open, while it is still loading.
  std::string reopen_url;
  double reopen_after = -1;
  // lychee_track_type, track index (-1 disables).
  std::vector<std::pair<int32_t, int32_t>> selections;
  double select_after = 2;
  bool hardware = true;
  bool virtual_time = false;
  // Where to write each subtitle with pictures, drawn on a grey canvas of
  // the video's size (binary PPM), to look at styled ASS.
  std::string subtitle_ppm;
  // Seconds after playback starts to make the video decoder fail as after a
  // GPU reset (testing.h), in order.
  std::vector<double> lose_decoder_after;
  // Stop pulling frames this many seconds after playback starts, as an app
  // in the background does, for |hide_for| seconds.
  double hide_after = -1;
  double hide_for = 2;
  // Live input: the file's packets pushed as they would arrive.
  bool live = false;
  int64_t latency_ms = 80;
  // Each packet arrives up to this much later than sent, in order.
  int64_t live_jitter_ms = 0;
  // Nothing arrives for |live_stall_for| seconds from |live_stall_at| on;
  // then it all arrives at once.
  double live_stall_at = -1;
  double live_stall_for = 0;
  // Flush both tracks this many seconds after the first packet.
  double live_flush_at = -1;
  // No video for this long, as from a sender whose screen does not
  // change; it resumes at a keyframe.
  double live_gap_at = -1;
  double live_gap_for = 0;
#if defined(__APPLE__)
  int32_t frame_type = LYCHEE_FRAME_CVPIXELBUFFER;
#else
  int32_t frame_type = LYCHEE_FRAME_CPU_RGBA;
#endif
};

std::atomic<int> g_seek_completed{0};
std::atomic<bool> g_media_info{false};
std::atomic<bool> g_tracks_changed{false};
std::atomic<bool> g_subtitle_changed{false};
std::atomic<bool> g_buffering{false};

const char* StateName(int32_t state) {
  switch (state) {
    case LYCHEE_STATE_IDLE:
      return "idle";
    case LYCHEE_STATE_LOADING:
      return "loading";
    case LYCHEE_STATE_READY:
      return "ready";
    case LYCHEE_STATE_BUFFERING:
      return "buffering";
    case LYCHEE_STATE_ENDED:
      return "ended";
    case LYCHEE_STATE_ERROR:
      return "error";
  }
  return "?";
}

void OnEvent(void*, int32_t type, int64_t arg0, int64_t arg1) {
  switch (type) {
    case LYCHEE_EVENT_STATE_CHANGED:
      printf("event: state %s\n", StateName(static_cast<int32_t>(arg0)));
      if (arg0 == LYCHEE_STATE_BUFFERING) {
        g_buffering = true;
      }
      break;
    case LYCHEE_EVENT_PLAYING_CHANGED:
      printf("event: playing %" PRId64 "\n", arg0);
      break;
    case LYCHEE_EVENT_MEDIA_INFO:
      printf("event: media info, duration %.3fs\n", arg0 / 1e6);
      g_media_info = true;
      break;
    case LYCHEE_EVENT_TRACKS_CHANGED:
      printf("event: tracks changed\n");
      g_tracks_changed = true;
      break;
    case LYCHEE_EVENT_SUBTITLE_CHANGED:
      g_subtitle_changed = true;
      break;
    case LYCHEE_EVENT_VIDEO_SIZE_CHANGED:
      printf("event: video size %" PRId64 "x%" PRId64 "\n", arg0, arg1);
      break;
    case LYCHEE_EVENT_SEEK_COMPLETED:
      printf("event: seek completed at %.3fs\n", arg0 / 1e6);
      g_seek_completed = 1;
      break;
    case LYCHEE_EVENT_ERROR:
      printf("event: error\n");
      break;
  }
  fflush(stdout);
}

bool ParseOptions(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto value = [&]() -> const char* {
      return i + 1 < argc ? argv[++i] : nullptr;
    };
    if (arg == "--seconds") {
      const char* v = value();
      if (!v) return false;
      options->seconds = atof(v);
    } else if (arg == "--seek") {
      const char* v = value();
      if (!v) return false;
      options->seek_to = atof(v);
    } else if (arg == "--seek-after") {
      const char* v = value();
      if (!v) return false;
      options->seek_after = atof(v);
    } else if (arg == "--volume") {
      const char* v = value();
      if (!v) return false;
      options->volume = static_cast<float>(atof(v));
    } else if (arg == "--refresh") {
      const char* v = value();
      if (!v || atof(v) <= 0) return false;
      options->refresh_rate = atof(v);
    } else if (arg == "--vsync-jitter") {
      const char* v = value();
      if (!v) return false;
      options->vsync_jitter = atoll(v);
    } else if (arg == "--rate") {
      const char* v = value();
      if (!v || atof(v) <= 0) return false;
      options->rate = atof(v);
    } else if (arg == "--rate-after") {
      const char* v = value();
      if (!v) return false;
      options->rate_after = atof(v);
    } else if (arg == "--loop") {
      options->loop = true;
    } else if (arg == "--loop-off-after") {
      const char* v = value();
      if (!v) return false;
      options->loop_off_after = atof(v);
    } else if (arg == "--open-after") {
      const char* v = value();
      const char* url = value();
      if (!v || !url) return false;
      options->reopen_after = atof(v);
      options->reopen_url = url;
    } else if (arg == "--select") {
      // TYPE:INDEX, TYPE one of video, audio, subtitle.
      const char* v = value();
      if (!v) return false;
      std::string spec = v;
      size_t colon = spec.find(':');
      if (colon == std::string::npos) return false;
      std::string type = spec.substr(0, colon);
      int32_t track_type = type == "video"      ? LYCHEE_TRACK_VIDEO
                           : type == "audio"    ? LYCHEE_TRACK_AUDIO
                           : type == "subtitle" ? LYCHEE_TRACK_SUBTITLE
                                                : 0;
      if (!track_type) return false;
      options->selections.emplace_back(track_type,
                                       atoi(spec.c_str() + colon + 1));
    } else if (arg == "--select-after") {
      const char* v = value();
      if (!v) return false;
      options->select_after = atof(v);
    } else if (arg == "--lose-decoder-after") {
      const char* v = value();
      if (!v) return false;
      options->lose_decoder_after.push_back(atof(v));
    } else if (arg == "--hide-after") {
      const char* v = value();
      if (!v) return false;
      options->hide_after = atof(v);
    } else if (arg == "--hide-for") {
      const char* v = value();
      if (!v) return false;
      options->hide_for = atof(v);
    } else if (arg == "--live") {
      options->live = true;
    } else if (arg == "--latency") {
      const char* v = value();
      if (!v) return false;
      options->latency_ms = atoll(v);
    } else if (arg == "--live-jitter") {
      const char* v = value();
      if (!v) return false;
      options->live_jitter_ms = atoll(v);
    } else if (arg == "--live-stall") {
      const char* at = value();
      const char* duration = value();
      if (!at || !duration) return false;
      options->live_stall_at = atof(at);
      options->live_stall_for = atof(duration);
    } else if (arg == "--live-flush") {
      const char* v = value();
      if (!v) return false;
      options->live_flush_at = atof(v);
    } else if (arg == "--live-gap") {
      const char* at = value();
      const char* duration = value();
      if (!at || !duration) return false;
      options->live_gap_at = atof(at);
      options->live_gap_for = atof(duration);
    } else if (arg == "--audio-only") {
      options->frame_type = 0;
    } else if (arg == "--subtitle-ppm") {
      const char* v = value();
      if (!v) return false;
      options->subtitle_ppm = v;
    } else if (arg == "--virtual-time") {
      options->virtual_time = true;
    } else if (arg == "--no-hw") {
      options->hardware = false;
    } else if (arg == "--cpu") {
      options->frame_type = LYCHEE_FRAME_CPU_BGRA;
    } else if (arg == "--rgba") {
      options->frame_type = LYCHEE_FRAME_CPU_RGBA;
    } else if (arg == "--native") {
      options->frame_type = LYCHEE_FRAME_CVPIXELBUFFER_NATIVE;
    } else if (arg == "--d3d11") {
      // CPU frames too, as the Flutter plugin takes them.
      options->frame_type = LYCHEE_FRAME_D3D11_TEXTURE | LYCHEE_FRAME_CPU_BGRA;
    } else if (!arg.empty() && arg[0] != '-') {
      options->url = arg;
    } else {
      return false;
    }
  }
  return !options->url.empty();
}

const char* TrackTypeName(int32_t type) {
  switch (type) {
    case LYCHEE_TRACK_VIDEO:
      return "video";
    case LYCHEE_TRACK_AUDIO:
      return "audio";
    case LYCHEE_TRACK_SUBTITLE:
      return "subtitle";
  }
  return "?";
}

void PrintTracks(lychee_player* player) {
  int32_t count = lychee_player_get_track_count(player);
  for (int32_t i = 0; i < count; ++i) {
    lychee_track_info info = {};
    info.struct_size = sizeof(info);
    if (lychee_player_get_track_info(player, i, &info) != 0) {
      continue;
    }
    printf("track %d: %s %s", i, TrackTypeName(info.type), info.codec);
    if (info.type == LYCHEE_TRACK_VIDEO) {
      printf(" %dx%d rotation=%d", info.width, info.height,
             lychee_player_get_video_rotation(player));
    } else if (info.type == LYCHEE_TRACK_AUDIO) {
      printf(" %d Hz %d ch", info.sample_rate, info.channels);
    }
    printf(" lang=%s title=%s selected=%d\n", info.language, info.title,
           info.selected);
  }
}

void PrintMetadata(lychee_player* player) {
  printf("metadata:");
  for (const char* key : {"title", "artist", "album"}) {
    char value[256];
    if (lychee_player_get_metadata(player, key, value, sizeof(value)) >= 0) {
      printf(" %s=%s", key, value);
    }
  }
  printf("\n");
  const int32_t size = lychee_player_get_artwork(player, nullptr, 0);
  if (size >= 0) {
    std::vector<uint8_t> artwork(size);
    lychee_player_get_artwork(player, artwork.data(), size);
    const bool png = size >= 4 && artwork[0] == 0x89 && artwork[1] == 'P';
    const bool jpeg = size >= 2 && artwork[0] == 0xff && artwork[1] == 0xd8;
    printf("artwork: %d bytes %s\n", size,
           png ? "png" : jpeg ? "jpeg" : "other");
  }
}

// Subtitle pictures on the video's own canvas: where each goes, and how
// much of it is opaque and in what colour, to tell text from boxes.
#if defined(_WIN32)
// The center pixel of a LYCHEE_FRAME_D3D11_TEXTURE frame, as R, G, B.
void ReadCenterPixel(ID3D11Device* device,
                     ID3D11DeviceContext* context,
                     lychee_frame* frame,
                     int* rgb) {
  using Microsoft::WRL::ComPtr;
  auto* texture =
      static_cast<ID3D11Texture2D*>(lychee_frame_get_native_handle(frame));
  ComPtr<IDXGIResource> resource;
  HANDLE shared = nullptr;
  ComPtr<ID3D11Texture2D> opened;
  if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) ||
      FAILED(resource->GetSharedHandle(&shared)) ||
      FAILED(device->OpenSharedResource(shared, IID_PPV_ARGS(&opened)))) {
    return;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = desc.Height = 1;
  desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_STAGING;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return;
  }
  const UINT x = static_cast<UINT>(lychee_frame_get_width(frame) / 2);
  const UINT y = static_cast<UINT>(lychee_frame_get_height(frame) / 2);
  const D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
  context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, opened.Get(), 0,
                                 &box);
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    const auto* p = static_cast<const uint8_t*>(mapped.pData);
    rgb[0] = p[2];
    rgb[1] = p[1];
    rgb[2] = p[0];
    context->Unmap(staging.Get(), 0);
  }
}
#endif

#if defined(__APPLE__)

// Pixel format and transfer function of a LYCHEE_FRAME_CVPIXELBUFFER_NATIVE
// frame.
void PrintNativeFrame(lychee_frame* frame) {
  auto buffer =
      static_cast<CVPixelBufferRef>(lychee_frame_get_native_handle(frame));
  const OSType format = CVPixelBufferGetPixelFormatType(buffer);
  char transfer[64] = "none";
  CFTypeRef value = CVBufferCopyAttachment(
      buffer, kCVImageBufferTransferFunctionKey, nullptr);
  if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
    CFStringGetCString(static_cast<CFStringRef>(value), transfer,
                       sizeof(transfer), kCFStringEncodingUTF8);
  }
  if (value) {
    CFRelease(value);
  }
  printf("native frame: %c%c%c%c transfer=%s\n", format >> 24 & 0xff,
         format >> 16 & 0xff, format >> 8 & 0xff, format & 0xff, transfer);
}
#endif

// The pictures of |subtitle| blended over grey, on the canvas they are
// placed on (the video's size), as a binary PPM.
void WriteSubtitlePpm(lychee_player* player,
                      lychee_subtitle* subtitle,
                      const std::string& path) {
  int32_t width = 0, height = 0;
  lychee_player_get_video_size(player, &width, &height);
  if (width <= 0 || height <= 0) {
    return;
  }
  std::vector<uint8_t> canvas(static_cast<size_t>(width) * height * 3, 128);
  const int32_t count = lychee_subtitle_get_image_count(subtitle);
  for (int32_t i = 0; i < count; ++i) {
    lychee_subtitle_image image = {};
    image.struct_size = sizeof(image);
    lychee_subtitle_get_image(subtitle, i, &image);
    for (int32_t y = 0; y < image.height; ++y) {
      for (int32_t x = 0; x < image.width; ++x) {
        const int32_t cx = image.x + x, cy = image.y + y;
        if (cx < 0 || cy < 0 || cx >= width || cy >= height) {
          continue;
        }
        // Nearest neighbour, should the picture be scaled.
        const uint8_t* p =
            image.data +
            static_cast<ptrdiff_t>(y * image.bitmap_height / image.height) * image.stride +
            (x * image.bitmap_width / image.width) * 4;
        uint8_t* out = canvas.data() + (static_cast<size_t>(cy) * width + cx) * 3;
        for (int c = 0; c < 3; ++c) {
          out[c] = static_cast<uint8_t>(p[c] + out[c] * (255 - p[3]) / 255);
        }
      }
    }
  }
  FILE* file = fopen(path.c_str(), "wb");
  if (!file) {
    return;
  }
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  fwrite(canvas.data(), 1, canvas.size(), file);
  fclose(file);
}

void PrintSubtitleImages(lychee_player* player, const std::string& ppm) {
  lychee_subtitle* subtitle = lychee_player_acquire_subtitle(player, 0, 0, 0);
  if (!subtitle) {
    return;
  }
  if (!ppm.empty()) {
    WriteSubtitlePpm(player, subtitle, ppm);
  }
  const int32_t count = lychee_subtitle_get_image_count(subtitle);
  printf("subtitle images: %d", count);
  for (int32_t i = 0; i < count; ++i) {
    lychee_subtitle_image image = {};
    image.struct_size = sizeof(image);
    lychee_subtitle_get_image(subtitle, i, &image);
    int64_t opaque = 0, r = 0, g = 0, b = 0;
    for (int32_t y = 0; y < image.bitmap_height; ++y) {
      const uint8_t* row = image.data + static_cast<ptrdiff_t>(y) * image.stride;
      for (int32_t x = 0; x < image.bitmap_width; ++x) {
        const uint8_t* p = row + x * 4;
        if (p[3] > 128) {
          opaque++;
          r += p[0];
          g += p[1];
          b += p[2];
        }
      }
    }
    const int64_t pixels =
        static_cast<int64_t>(image.bitmap_width) * image.bitmap_height;
    printf(" | %dx%d at %d,%d opaque=%" PRId64 "%% rgb=%" PRId64 ",%" PRId64
           ",%" PRId64,
           image.width, image.height, image.x, image.y,
           pixels ? opaque * 100 / pixels : 0, opaque ? r / opaque : 0,
           opaque ? g / opaque : 0, opaque ? b / opaque : 0);
  }
  printf("\n");
  lychee_subtitle_release(subtitle);
}

// Subtitle lines joined with " | ", so a cue stays on one output line.
std::string SubtitleText(lychee_player* player) {
  char text[1024];
  lychee_player_get_subtitle(player, 0, text, sizeof(text));
  std::string result;
  for (const char* c = text; *c; ++c) {
    if (*c == '\n') {
      result += " | ";
    } else {
      result += *c;
    }
  }
  return result;
}

// Pushes a file's packets into a live input at the times a sender would
// send them, plus the arrival delays the options ask for.
class LiveFeed {
 public:
  LiveFeed(const Options& options, lychee_player* player)
      : options_(options), player_(player), random_(2) {}
  ~LiveFeed() {
    if (source_) {
      lychee_testing_live_source_close(source_);
    }
  }

  bool Open(bool video) {
    source_ = lychee_testing_live_source_open(options_.url.c_str());
    if (!source_) {
      return false;
    }
    lychee_live_config config = {};
    config.struct_size = sizeof(config);
    config.latency_us = options_.latency_ms * 1000;
    lychee_player_open_live(player_, &config);
    for (int32_t type : {LYCHEE_TRACK_VIDEO, LYCHEE_TRACK_AUDIO}) {
      lychee_live_track track = {};
      track.struct_size = sizeof(track);
      if ((type == LYCHEE_TRACK_VIDEO && !video) ||
          lychee_testing_live_source_track(source_, type, &track) != 0) {
        continue;
      }
      printf("live track: %s codec %d\n", TrackTypeName(type), track.codec);
      lychee_player_set_live_track(player_, &track);
    }
    return true;
  }

  // Pushes what has arrived by |now|. False once everything is pushed.
  bool Feed(int64_t now) {
    for (;;) {
      if (!pending_ && !Next()) {
        return false;
      }
      if (start_ == INT64_MIN) {
        start_ = now;
        first_send_ = send_time_;
      }
      const int64_t since = send_time_ - first_send_;
      if (!flushed_ && options_.live_flush_at >= 0 &&
          since >= static_cast<int64_t>(options_.live_flush_at * 1e6)) {
        flushed_ = true;
        printf("live flush at %.3fs\n", since / 1e6);
        lychee_player_flush_live(player_, 0);
      }
      int64_t arrival = start_ + since;
      if (options_.live_jitter_ms > 0) {
        arrival += std::uniform_int_distribution<int64_t>(
            0, options_.live_jitter_ms * 1000)(random_);
      }
      const int64_t stall_at =
          static_cast<int64_t>(options_.live_stall_at * 1e6);
      const int64_t stall_end =
          stall_at + static_cast<int64_t>(options_.live_stall_for * 1e6);
      if (options_.live_stall_at >= 0 && since >= stall_at &&
          since < stall_end) {
        arrival = std::max(arrival, start_ + stall_end);
      }
      arrival = std::max(arrival, last_arrival_);
      if (arrival > now) {
        return true;
      }
      last_arrival_ = arrival;
      pending_ = false;
      if (packet_.type == LYCHEE_TRACK_VIDEO && InGap(since)) {
        continue;
      }
      packet_.data = data_.data();
      packet_.size = static_cast<int32_t>(data_.size());
      lychee_player_push_live_packet(player_, &packet_);
    }
  }

 private:
  bool Next() {
    packet_ = {};
    packet_.struct_size = sizeof(packet_);
    if (lychee_testing_live_source_read(source_, &packet_, &send_time_) != 0) {
      return false;
    }
    data_.assign(packet_.data, packet_.data + packet_.size);
    pending_ = true;
    return true;
  }

  // Whether video sent at |since| falls in the gap: from its start up to
  // the first keyframe after its end.
  bool InGap(int64_t since) {
    if (options_.live_gap_at < 0 || gap_done_) {
      return false;
    }
    const int64_t gap_at = static_cast<int64_t>(options_.live_gap_at * 1e6);
    const int64_t gap_end =
        gap_at + static_cast<int64_t>(options_.live_gap_for * 1e6);
    if (since < gap_at) {
      return false;
    }
    if (!in_gap_) {
      in_gap_ = true;
      printf("live video gap at %.3fs\n", since / 1e6);
    }
    if (since >= gap_end && IsKeyframe()) {
      gap_done_ = true;
      printf("live video resumes at %.3fs\n", since / 1e6);
      return false;
    }
    return true;
  }

  // The live source puts parameter sets before every keyframe.
  bool IsKeyframe() const {
    for (size_t i = 0; i + 4 < data_.size(); ++i) {
      if (data_[i] == 0 && data_[i + 1] == 0 && data_[i + 2] == 1) {
        const uint8_t h264 = data_[i + 3] & 0x1f;
        const uint8_t hevc = (data_[i + 3] >> 1) & 0x3f;
        if (h264 == 7 || hevc == 32 || hevc == 33) {
          return true;
        }
      }
    }
    return false;
  }

  const Options& options_;
  lychee_player* const player_;
  lychee_testing_live_source* source_ = nullptr;
  std::mt19937 random_;
  lychee_live_packet packet_ = {};
  std::vector<uint8_t> data_;
  int64_t send_time_ = 0;
  bool pending_ = false;
  int64_t start_ = INT64_MIN;
  int64_t first_send_ = 0;
  int64_t last_arrival_ = INT64_MIN;
  bool flushed_ = false;
  bool in_gap_ = false;
  bool gap_done_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    fprintf(stderr,
            "usage: %s [--seconds N] [--seek SEC [--seek-after SEC]] "
            "[--volume 0..1] [--refresh HZ] [--vsync-jitter US] [--no-hw] "
            "[--cpu | --rgba | --native | --d3d11] [--rate R [--rate-after SEC]] [--loop [--loop-off-after SEC]] "
            "[--open-after SEC URL] [--select TYPE:INDEX]... "
            "[--select-after SEC] [--lose-decoder-after SEC]... "
            "[--hide-after SEC [--hide-for SEC]] [--audio-only] "
            "[--live [--latency MS] [--live-jitter MS] "
            "[--live-stall SEC FOR] [--live-flush SEC] [--live-gap SEC FOR]] "
            "[--subtitle-ppm FILE] [--virtual-time] <file or url>\n",
            argv[0]);
    return 2;
  }

  if (options.virtual_time) {
    lychee_testing_enable_virtual_time();
  }
  lychee_config config;
  lychee_config_init(&config, sizeof(config));
  config.video_frame_types = options.frame_type;
  config.hardware_decoding = options.hardware ? 1 : 0;
#if defined(_WIN32)
  // --d3d11: shared textures on the first adapter, read back through a
  // device of our own as the Flutter plugin opens them.
  Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
  Microsoft::WRL::ComPtr<ID3D11Device> device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  lychee_video_device video_device = {sizeof(video_device),
                                      LYCHEE_VIDEO_DEVICE_D3D11_ADAPTER,
                                      nullptr, nullptr};
  if (options.frame_type & LYCHEE_FRAME_D3D11_TEXTURE) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapters(0, &adapter)) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
      fprintf(stderr, "no D3D11 device\n");
      return 1;
    }
    video_device.handle = adapter.Get();
    config.video_device = &video_device;
  }
#endif

  lychee_player* player = lychee_player_create(&config);
  lychee_player_set_event_callback(player, &OnEvent, nullptr);
  lychee_player_set_volume(player, options.volume);
  lychee_player_set_looping(player, options.loop ? 1 : 0);
  if (options.rate_after < 0) {
    lychee_player_set_rate(player, options.rate);
  }
  std::unique_ptr<LiveFeed> live;
  if (options.live) {
    live = std::make_unique<LiveFeed>(options, player);
    if (!live->Open(options.frame_type != 0)) {
      fprintf(stderr, "cannot read %s\n", options.url.c_str());
      return 1;
    }
  } else if (lychee_player_open(player, options.url.c_str()) != 0) {
    fprintf(stderr, "open rejected\n");
    return 1;
  }
  lychee_player_play(player);
  bool reopened = false;
  if (options.reopen_after == 0) {
    printf("open -> %s\n", options.reopen_url.c_str());
    lychee_player_open(player, options.reopen_url.c_str());
    reopened = true;
  }

  const int64_t vsync_interval =
      static_cast<int64_t>(1e6 / options.refresh_rate);
  std::mt19937 random(1);
  std::uniform_int_distribution<int64_t> jitter(-options.vsync_jitter,
                                                options.vsync_jitter);
  const int64_t start = lychee_now_us();
  int64_t next_vsync = start;
  int64_t next_report = start + 1000000;
  int64_t last_pts = INT64_MIN;
  int64_t backward_jumps = 0;
  int64_t frames_pulled = 0;
  bool seek_issued = false;
  int64_t seek_issued_at = 0;
  int64_t first_pts_after_seek = INT64_MIN;
  int64_t ready_wall = 0;
  // Wall clock vs. media clock since playback last (re)started, to spot
  // drift.
  int64_t drift_wall = 0;
  int64_t drift_position = 0;
  // After (re)starting, the position holds still until the audio device
  // starts playing, ~300 ms on Bluetooth: measure drift from when it moves.
  bool drift_wait_motion = false;
  int32_t last_state = -1;
  // Smoothness: a frame lasting D on a display refreshing every I should stay
  // on screen floor(D/I) or ceil(D/I) vsyncs. Any other count is a glitch (a
  // frame repeated or cut short), even when no frame was dropped. Counted
  // from a second after playback starts: from jittered display times the
  // library needs that long to estimate the refresh rate well enough for a
  // cadence.
  constexpr int64_t kSettleTime = 1000000;
  int64_t glitches = 0;
  int64_t vsyncs_on_screen = 0;
  int32_t frame_type = 0;
  // Of the last CPU frame, R, G, B; -1 if none.
  int center_pixel[3] = {-1, -1, -1};
  // The frame on screen has been shown only while playing steadily.
  bool steady = false;
  int exit_code = 0;
  double rate = options.rate_after < 0 ? options.rate : 1;
  bool rate_applied = options.rate_after < 0;
  // Largest |drift| reported, a second after playback (re)starts.
  int64_t max_drift = 0;
  int64_t loops = 0;
  bool selected = options.selections.empty();
  // Position when the track change was requested; INT64_MIN once reported.
  int64_t select_position = INT64_MIN;
  bool tracks_changed = false;
  int64_t loop_check_position = 0;
  // A jump back to the start is a loop only while looping is on.
  bool looping = options.loop;
  size_t next_loss = 0;
  // Wall time the hidden stretch ends; 0 if not hidden (yet), -1 after.
  int64_t hidden_until = 0;
  // Position when the decoder was lost; INT64_MIN once the rebuild resumed.
  int64_t loss_position = INT64_MIN;

  // Virtual time only: vsyncs where the player's threads did not catch up
  // within kStallTimeout of real time. After kMaxStalls the pipeline is
  // taken to be stuck, and the run stops.
  constexpr auto kStallTimeout = std::chrono::seconds(2);
  constexpr int64_t kMaxStalls = 5;
  int64_t stalls = 0;

  // Live: when the last packet was pushed, and how far ahead of then the
  // decoded media reached: the latency in use.
  int64_t live_end = 0;
  int64_t live_vbuf = -1;
  int64_t live_abuf = -1;

  for (;;) {
    next_vsync += vsync_interval;
    if (live) {
      // Packets arrive at their time; then the threads decode them.
      if (options.virtual_time) {
        lychee_testing_set_time(next_vsync);
      } else {
        std::this_thread::sleep_for(
            std::chrono::microseconds(next_vsync - lychee_now_us()));
      }
      if (!live->Feed(lychee_now_us()) && live_end == 0) {
        live_end = lychee_now_us();
      }
      const auto give_up = std::chrono::steady_clock::now() + kStallTimeout;
      while (options.virtual_time &&
             !lychee_testing_player_settled(player,
                                            next_vsync + vsync_interval)) {
        if (std::chrono::steady_clock::now() > give_up) {
          printf("stall: threads not settled at %.3fs\n",
                 (next_vsync - start) / 1e6);
          stalls++;
          break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      if (live_end != 0 && live_vbuf < 0) {
        lychee_stats stats = {};
        stats.struct_size = sizeof(stats);
        lychee_player_get_stats(player, &stats);
        live_vbuf = stats.video_buffered_us;
        live_abuf = stats.audio_buffered_us;
      }
    } else if (options.virtual_time) {
      // Let the threads decode up to the frame this vsync will pick and the
      // audio a device would buffer (kAudioAhead), then jump there.
      constexpr int64_t kAudioAhead = 50000;
      const int64_t until = next_vsync + vsync_interval + kAudioAhead;
      const auto give_up = std::chrono::steady_clock::now() + kStallTimeout;
      while (!lychee_testing_player_settled(player, until)) {
        if (std::chrono::steady_clock::now() > give_up) {
          printf("stall: threads not settled at %.3fs\n",
                 (next_vsync - start) / 1e6);
          stalls++;
          break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      lychee_testing_set_time(next_vsync);
    } else {
      std::this_thread::sleep_for(
          std::chrono::microseconds(next_vsync - lychee_now_us()));
    }
    int64_t now = lychee_now_us();
    // Like a real display link: regular vsync timestamps, independent of how
    // late this thread woke up, give or take the timestamp jitter.
    int64_t display_time = next_vsync + vsync_interval + jitter(random);

    bool playing_steadily = last_state == LYCHEE_STATE_READY;
    if (hidden_until == 0 && options.hide_after >= 0 && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.hide_after * 1e6)) {
      hidden_until = now + static_cast<int64_t>(options.hide_for * 1e6);
      printf("hide at %.3fs\n", lychee_player_get_position(player) / 1e6);
    }
    if (hidden_until > 0 && now >= hidden_until) {
      hidden_until = -1;
      steady = false;
      printf("show at %.3fs\n", lychee_player_get_position(player) / 1e6);
    }
    if (hidden_until > 0) {
      // Nothing pulls frames.
    } else if (lychee_player_has_new_frame(player, display_time)) {
      lychee_frame* frame = lychee_player_acquire_frame(player, display_time);
      if (frame) {
        int64_t pts = lychee_frame_get_pts(frame);
        frame_type = lychee_frame_get_type(frame);
#if defined(__APPLE__)
        if (frame_type == LYCHEE_FRAME_CVPIXELBUFFER_NATIVE &&
            frames_pulled == 0) {
          PrintNativeFrame(frame);
        }
#endif
#if defined(_WIN32)
        if (frame_type == LYCHEE_FRAME_D3D11_TEXTURE &&
            frames_pulled % 30 == 0) {
          ReadCenterPixel(device.Get(), context.Get(), frame, center_pixel);
        }
#endif
        if (frame_type == LYCHEE_FRAME_CPU_BGRA ||
            frame_type == LYCHEE_FRAME_CPU_RGBA) {
          int32_t stride = 0;
          const uint8_t* data = lychee_frame_get_data(frame, &stride);
          const uint8_t* p =
              data +
              static_cast<ptrdiff_t>(lychee_frame_get_height(frame) / 2) *
                  stride +
              lychee_frame_get_width(frame) / 2 * 4;
          const bool bgra = frame_type == LYCHEE_FRAME_CPU_BGRA;
          center_pixel[0] = p[bgra ? 2 : 0];
          center_pixel[1] = p[1];
          center_pixel[2] = p[bgra ? 0 : 2];
        }
        // The first discontinuity after a seek is the seek landing.
        bool discontinuity =
            last_pts != INT64_MIN &&
            (pts < last_pts ||
             pts > last_pts + static_cast<int64_t>(500000 * rate));
        bool settled = ready_wall != 0 && now - ready_wall >= kSettleTime;
        if (steady && settled && !discontinuity && pts > last_pts) {
          double ideal =
              static_cast<double>(pts - last_pts) / rate / vsync_interval;
          if (vsyncs_on_screen < static_cast<int64_t>(ideal + 0.02) ||
              vsyncs_on_screen > static_cast<int64_t>(ideal - 0.02) + 1) {
            glitches++;
            printf("glitch: %.3fs held %" PRId64 " vsyncs, expected %.2f\n",
                   last_pts / 1e6, vsyncs_on_screen, ideal);
          }
        }
        steady = playing_steadily;
        vsyncs_on_screen = 0;
        if (seek_issued && first_pts_after_seek == INT64_MIN &&
            discontinuity) {
          first_pts_after_seek = pts;
        } else if (looping && last_pts != INT64_MIN &&
                   pts < last_pts - 1000000) {
          // Looped back to the start.
        } else if (last_pts != INT64_MIN && pts < last_pts) {
          backward_jumps++;
        }
        last_pts = pts;
        frames_pulled++;
        lychee_frame_release(frame);
      }
    }
    vsyncs_on_screen++;
    steady = steady && playing_steadily;

    int32_t state = lychee_player_get_state(player);
    int64_t position = lychee_player_get_position(player);
    if (state == LYCHEE_STATE_READY && ready_wall == 0) {
      ready_wall = now;
    }
    if (state == LYCHEE_STATE_READY && last_state != LYCHEE_STATE_READY) {
      drift_wall = now;
      drift_position = position;
      drift_wait_motion = true;
    } else if (drift_wait_motion && position != drift_position) {
      drift_wall = now;
      drift_position = position;
      drift_wait_motion = false;
    }
    last_state = state;

    if (g_media_info.exchange(false)) {
      printf("seekable=%d\n", lychee_player_is_seekable(player));
      PrintTracks(player);
      PrintMetadata(player);
    }
    if (g_subtitle_changed.exchange(false)) {
      std::string text = SubtitleText(player);
      printf("subtitle at %.3fs: %s\n", position / 1e6,
             text.empty() ? "(none)" : text.c_str());
      PrintSubtitleImages(player, options.subtitle_ppm);
    }
    if (options.loop && state == LYCHEE_STATE_READY) {
      if (position < loop_check_position - 1000000) {
        loops++;
        printf("looped at %.3fs\n", loop_check_position / 1e6);
        drift_wall = now;
        drift_position = position;
      }
      loop_check_position = position;
    }
    if (looping && options.loop_off_after >= 0 && ready_wall != 0 &&
        now - ready_wall >=
            static_cast<int64_t>(options.loop_off_after * 1e6)) {
      looping = false;
      printf("loop off at %.3fs\n", position / 1e6);
      lychee_player_set_looping(player, 0);
    }
    if (!rate_applied && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.rate_after * 1e6)) {
      rate_applied = true;
      printf("rate -> %.2f\n", options.rate);
      lychee_player_set_rate(player, options.rate);
      rate = lychee_player_get_rate(player);
      // Drift is measured per rate.
      drift_wall = now;
      drift_position = position;
    }
    if (!selected && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.select_after * 1e6)) {
      selected = true;
      select_position = position;
      for (const auto& [type, index] : options.selections) {
        int32_t result = lychee_player_select_track(player, type, index);
        printf("select %s %d at %.3fs: %s\n", TrackTypeName(type), index,
               position / 1e6, result == 0 ? "accepted" : "rejected");
      }
    }
    if (g_tracks_changed.exchange(false)) {
      tracks_changed = true;
      PrintTracks(player);
    }
    if (tracks_changed && select_position != INT64_MIN &&
        state == LYCHEE_STATE_READY) {
      printf("track switch: resumed %+.3fs from %.3fs\n",
             (position - select_position) / 1e6, select_position / 1e6);
      select_position = INT64_MIN;
      // The switch rebuffers; measure drift from here.
      drift_wall = now;
      drift_position = position;
    }
    if (next_loss < options.lose_decoder_after.size() &&
        loss_position == INT64_MIN && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(
                                options.lose_decoder_after[next_loss] * 1e6)) {
      next_loss++;
      loss_position = position;
      g_buffering = false;
      printf("lose video decoder at %.3fs\n", position / 1e6);
      lychee_testing_lose_video_decoder(player);
    }
    if (loss_position != INT64_MIN && g_buffering &&
        state == LYCHEE_STATE_READY) {
      printf("video rebuilt: resumed %+.3fs from %.3fs\n",
             (position - loss_position) / 1e6, loss_position / 1e6);
      loss_position = INT64_MIN;
      drift_wall = now;
      drift_position = position;
    }
    if (!reopened && options.reopen_after > 0 && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.reopen_after * 1e6)) {
      reopened = true;
      printf("open -> %s\n", options.reopen_url.c_str());
      lychee_player_open(player, options.reopen_url.c_str());
      // A new timeline.
      last_pts = INT64_MIN;
      ready_wall = 0;
      steady = false;
      last_state = -1;
      continue;
    }

    if (options.seek_to >= 0 && !seek_issued && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.seek_after * 1e6)) {
      printf("seek -> %.3fs\n", options.seek_to);
      lychee_player_seek(player, static_cast<int64_t>(options.seek_to * 1e6));
      seek_issued = true;
      seek_issued_at = now;
    }
    if (seek_issued && g_seek_completed == 1) {
      g_seek_completed = 2;
      printf("seek took %.1f ms\n", (now - seek_issued_at) / 1e3);
      // Restart drift measurement from the seek target.
      drift_wall = now;
      drift_position = position;
    }

    if (now >= next_report) {
      next_report += 1000000;
      lychee_stats stats = {};
      stats.struct_size = sizeof(stats);
      lychee_player_get_stats(player, &stats);
      int64_t drift = 0;
      if (drift_wall && state == LYCHEE_STATE_READY) {
        drift = (position - drift_position) -
                static_cast<int64_t>((now - drift_wall) * rate);
        if (now - drift_wall >= kSettleTime) {
          max_drift = std::max(max_drift, std::abs(drift));
        }
      }
      printf(
          "t=%5.1fs state=%-9s pos=%7.3fs buffered=%7.3fs decoder=%s hw=%d "
          "rendered=%" PRId64 " dropped=%" PRId64
          " drift=%+.1fms vbuf=%.2fs abuf=%.2fs vdec=%.2fms\n",
          (now - start) / 1e6, StateName(state), position / 1e6,
          lychee_player_get_buffered_position(player) / 1e6,
          stats.video_decoder, stats.hardware_decoding, stats.frames_rendered,
          stats.frames_dropped, drift / 1e3, stats.video_buffered_us / 1e6,
          stats.audio_buffered_us / 1e6, stats.video_decode_time_us / 1e3);
      fflush(stdout);
    }

    if (state == LYCHEE_STATE_ERROR) {
      char error[512];
      lychee_player_get_error(player, error, sizeof(error));
      fprintf(stderr, "error: %s\n", error);
      exit_code = 1;
      break;
    }
    if (state == LYCHEE_STATE_ENDED) {
      break;
    }
    if (stalls >= kMaxStalls) {
      printf("stall: giving up\n");
      break;
    }
    // A live input never ends: stop a second after the last packet.
    if (live_end != 0 && now - live_end >= 1000000) {
      break;
    }
    if (options.seconds > 0 &&
        now - start >= static_cast<int64_t>(options.seconds * 1e6)) {
      break;
    }
  }

  lychee_stats stats = {};
  stats.struct_size = sizeof(stats);
  lychee_player_get_stats(player, &stats);
  printf("summary: state=%s position=%.3fs decoder=%s hw=%d pulled=%" PRId64
         " rendered=%" PRId64 " dropped=%" PRId64 " glitches=%" PRId64
         " backward_jumps=%" PRId64 " frame_type=%d rate=%.2f"
         " max_drift=%.1fms loops=%" PRId64 " stalls=%" PRId64,
         StateName(lychee_player_get_state(player)),
         lychee_player_get_position(player) / 1e6, stats.video_decoder,
         stats.hardware_decoding, frames_pulled, stats.frames_rendered,
         stats.frames_dropped, glitches, backward_jumps, frame_type,
         lychee_player_get_rate(player), max_drift / 1e3, loops, stalls);
  if (seek_issued && first_pts_after_seek != INT64_MIN) {
    printf(" first_pts_after_seek=%.3fs", first_pts_after_seek / 1e6);
  }
  if (live) {
    printf(" live_vbuf=%.3fs live_abuf=%.3fs", live_vbuf / 1e6,
           live_abuf / 1e6);
  }
  printf("\n");
  if (center_pixel[0] >= 0) {
    printf("center pixel: %d,%d,%d\n", center_pixel[0], center_pixel[1],
           center_pixel[2]);
  }
  lychee_player_destroy(player);
  return exit_code;
}
