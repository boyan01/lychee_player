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
#include <cmath>
#include <cstdarg>
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
#include <VideoToolbox/VideoToolbox.h>
#elif defined(_WIN32)
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#elif defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace {

// printf() into |out|.
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void Appendf(std::string* out, const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list copy;
  va_copy(copy, args);
  const int size = vsnprintf(nullptr, 0, format, copy);
  va_end(copy);
  if (size > 0) {
    const size_t old = out->size();
    out->resize(old + static_cast<size_t>(size) + 1);
    vsnprintf(&(*out)[old], static_cast<size_t>(size) + 1, format, args);
    out->resize(old + static_cast<size_t>(size));
  }
  va_end(args);
}

// The library's log lines go to stdout with the CLI's own, each in one
// write, so neither lands in the middle of the other's line (stderr is
// unbuffered and stdout is flushed whenever its buffer fills).
void LogToStdout(void*, int32_t, const char* message) {
  fprintf(stdout, "%s\n", message);
  fflush(stdout);
}

// --frame-ppm SEC:FILE: the frame on screen at SEC seconds of the run.
struct FrameSnapshot {
  double at;
  std::string path;
};

// --fade SEC:VOLUME:MS.
struct FadeAction {
  double at;
  float volume;
  int64_t duration_us;
};

// --audio-samples SEC:FRAMES.
struct SamplesPull {
  double at;
  int frames;
};

// --select TYPE:INDEX[@SEC].
struct Selection {
  int32_t type;   // lychee_track_type
  int32_t index;  // -1 disables the type.
  // Seconds after playback starts; < 0: --select-after.
  double after;
};

struct Options {
  double refresh_rate = 60;
  // Random error added to each vsync timestamp, in microseconds.
  int64_t vsync_jitter = 0;
  std::string url;
  double seconds = 0;
  double seek_to = -1;
  double seek_after = 2;
  float volume = 0;
  // --gain DB[:PEAK], set before the open.
  float gain_db = 0;
  float gain_peak = 0;
  // Fades and pulls of the samples played, by seconds after playback
  // starts.
  std::vector<FadeAction> fades;
  std::vector<SamplesPull> sample_pulls;
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
  // Close what is open this many seconds after playback starts, then open
  // |url| again a second later.
  double close_after = -1;
  // Seek right after the open, while loading: where playback starts.
  double start_at = -1;
  // Switch to |switch_url| (the same content) at the position this many
  // seconds after playback starts.
  std::string switch_url;
  double switch_after = -1;
  // The item to play when |url| ends (lychee_player_set_next()), and when
  // to unset it again, in seconds after playback starts.
  std::string next_url;
  double clear_next_after = -1;
  // Soak: every |soak_every| seconds of playback, seek to a random
  // position, twice, then open the other of |url| and |soak_url|.
  double soak_every = -1;
  std::string soak_url;
  std::vector<Selection> selections;
  double select_after = 2;
  // --variant INDEX@SEC: lychee_player_select_variant() SEC seconds after
  // playback starts (-1: automatic again).
  std::vector<std::pair<int32_t, double>> variants;
  // --max-bitrate BPS[@SEC]: before the open, or SEC seconds after
  // playback starts.
  int64_t max_bitrate = -1;
  double max_bitrate_after = -1;
  // --seek-edge SEC: a seek to the live edge SEC seconds after playback
  // starts.
  double seek_edge_after = -1;
  bool hardware = true;
  // --dmabuf: the DRM render node to decode on.
  std::string dmabuf_node;
  bool virtual_time = false;
  // Where to write what the silent audio device played (testing.h), as a
  // WAV for tool/audio_check.py; needs --virtual-time or --null-audio, and
  // a volume above 0.
  std::string audio_dump;
  // Pause this many seconds after playback starts, for |pause_for|.
  double pause_at = -1;
  double pause_for = 0;
  // A silent audio device pulled on its own thread, even without a sound card.
  bool null_audio = false;
  // Logs at LYCHEE_LOG_DEBUG: state changes, stats every 5 s.
  bool verbose = false;
  // Where to write each subtitle with pictures, drawn on a grey canvas of
  // the video's size (binary PPM), to look at bitmap subtitles.
  std::string subtitle_ppm;
  // Subtitle files to add right after the open, in this encoding (empty:
  // guessed).
  std::vector<std::string> add_subtitles;
  std::string subtitle_encoding;
  // Add them without selecting them.
  bool subtitle_keep_selection = false;
  // HTTP request headers for every URL opened, subtitles included:
  // "Name: value" lines.
  std::string headers;
  // Seconds to show subtitles later (negative: earlier).
  double subtitle_delay = 0;
  // Frames to write as they would be shown (tool/frame_check.py), by time.
  std::vector<FrameSnapshot> snapshots;
  // Seconds after playback starts to make the video decoder fail as after a
  // GPU reset, or hang as in a driver that never returns (testing.h), in
  // order.
  struct DecoderLoss {
    double after;
    bool hang;
    bool rebuild = false;
  };
  std::vector<DecoderLoss> lose_decoder_after;
  // Seconds after playback starts to stop the audio device pulling audio
  // (testing.h); 0 or less: from the start, as a device that never starts.
  // It pulls again |stall_audio_for| seconds later, if set.
  double stall_audio_after = -1;
  double stall_audio_for = -1;
  bool stall_audio = false;
  // The first |fail_audio_starts| audio device starts fail.
  int fail_audio_starts = 0;
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
  // No audio for this long, as from a sender that stops sending in
  // silence; its timestamps jump over the gap.
  double live_audio_gap_at = -1;
  double live_audio_gap_for = 0;
  double live_audio_jump_at = -1;
#if defined(__APPLE__)
  int32_t frame_type = LYCHEE_FRAME_CVPIXELBUFFER;
#else
  int32_t frame_type = LYCHEE_FRAME_CPU_RGBA;
#endif
};

std::atomic<int> g_seek_completed{0};
// LYCHEE_EVENT_VARIANT_CHANGED's index, or -1.
std::atomic<int> g_variant_changed{-1};
std::atomic<int> g_item_changes{0};
// The duration last reported, and at each item change the one of the item
// before, and whether it joined without a gap.
std::atomic<int64_t> g_duration{0};
std::atomic<int64_t> g_item_duration{0};
std::atomic<bool> g_item_gapless{false};
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
      g_duration = arg0;
      g_media_info = true;
      break;
    case LYCHEE_EVENT_TRACKS_CHANGED:
      printf("event: tracks changed\n");
      g_tracks_changed = true;
      break;
    case LYCHEE_EVENT_SUBTITLE_CHANGED:
      g_subtitle_changed = true;
      break;
    case LYCHEE_EVENT_SUBTITLE_ADDED:
      printf("event: subtitle %" PRId64 " %s\n", arg0,
             arg1 ? "added" : "failed");
      break;
    case LYCHEE_EVENT_VIDEO_SIZE_CHANGED:
      printf("event: video size %" PRId64 "x%" PRId64 "\n", arg0, arg1);
      break;
    case LYCHEE_EVENT_VARIANT_CHANGED:
      g_variant_changed = static_cast<int>(arg0);
      break;
    case LYCHEE_EVENT_SEEK_COMPLETED:
      printf("event: seek completed at %.3fs\n", arg0 / 1e6);
      g_seek_completed = 1;
      break;
    case LYCHEE_EVENT_ERROR:
      printf("event: error\n");
      break;
    case LYCHEE_EVENT_ITEM_CHANGED:
      printf("event: item changed, gapless %" PRId64 "\n", arg0);
      // Before the new item's LYCHEE_EVENT_MEDIA_INFO.
      g_item_duration = g_duration.load();
      g_item_gapless = arg0 != 0;
      g_item_changes++;
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
    } else if (arg == "--gain") {
      const char* v = value();
      if (!v) return false;
      char* end = nullptr;
      options->gain_db = strtof(v, &end);
      if (end == v) return false;
      if (*end == ':') {
        options->gain_peak = strtof(end + 1, nullptr);
      }
    } else if (arg == "--fade") {
      // SEC:VOLUME:MS.
      const char* v = value();
      if (!v) return false;
      char* end = nullptr;
      FadeAction fade{};
      fade.at = strtod(v, &end);
      if (*end != ':') return false;
      fade.volume = strtof(end + 1, &end);
      if (*end != ':') return false;
      fade.duration_us = static_cast<int64_t>(strtod(end + 1, &end) * 1000);
      options->fades.push_back(fade);
    } else if (arg == "--audio-samples") {
      // SEC:FRAMES.
      const char* v = value();
      if (!v) return false;
      char* end = nullptr;
      SamplesPull pull{};
      pull.at = strtod(v, &end);
      if (*end != ':') return false;
      pull.frames = static_cast<int>(strtol(end + 1, &end, 10));
      if (pull.frames <= 0) return false;
      options->sample_pulls.push_back(pull);
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
    } else if (arg == "--switch-after") {
      const char* v = value();
      const char* url = value();
      if (!v || !url) return false;
      options->switch_after = strtod(v, nullptr);
      options->switch_url = url;
    } else if (arg == "--next") {
      const char* url = value();
      if (!url) return false;
      options->next_url = url;
    } else if (arg == "--clear-next-after") {
      const char* v = value();
      if (!v) return false;
      options->clear_next_after = strtod(v, nullptr);
    } else if (arg == "--start") {
      const char* v = value();
      if (!v) return false;
      options->start_at = strtod(v, nullptr);
    } else if (arg == "--close-after") {
      const char* v = value();
      if (!v) return false;
      options->close_after = strtod(v, nullptr);
    } else if (arg == "--soak") {
      const char* v = value();
      const char* url = value();
      char* end = nullptr;
      const double every = v ? strtod(v, &end) : 0;
      if (!url || end == v || *end != '\0' || every <= 0) return false;
      options->soak_every = every;
      options->soak_url = url;
    } else if (arg == "--select") {
      // TYPE:INDEX[@SEC], TYPE one of video, audio, subtitle.
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
      size_t at = spec.find('@', colon);
      options->selections.push_back(
          {track_type, atoi(spec.c_str() + colon + 1),
           at == std::string::npos ? -1 : atof(spec.c_str() + at + 1)});
    } else if (arg == "--variant") {
      const char* v = value();
      const char* at = v ? strchr(v, '@') : nullptr;
      if (!at) return false;
      options->variants.emplace_back(static_cast<int>(strtol(v, nullptr, 10)),
                                     strtod(at + 1, nullptr));
    } else if (arg == "--max-bitrate") {
      const char* v = value();
      if (!v) return false;
      options->max_bitrate = strtoll(v, nullptr, 10);
      const char* at = strchr(v, '@');
      options->max_bitrate_after = at ? strtod(at + 1, nullptr) : -1;
    } else if (arg == "--seek-edge") {
      const char* v = value();
      if (!v) return false;
      options->seek_edge_after = strtod(v, nullptr);
    } else if (arg == "--select-after") {
      const char* v = value();
      if (!v) return false;
      options->select_after = atof(v);
    } else if (arg == "--lose-decoder-after" || arg == "--hang-decoder-after" ||
               arg == "--rebuild-decoder-after") {
      const char* v = value();
      if (!v) return false;
      char* end = nullptr;
      const double after = std::strtod(v, &end);
      if (end == v || *end != '\0' || !std::isfinite(after) || after < 0) {
        return false;
      }
      options->lose_decoder_after.push_back({after,
                                             arg == "--hang-decoder-after",
                                             arg == "--rebuild-decoder-after"});
    } else if (arg == "--stall-audio-after") {
      const char* v = value();
      if (!v) return false;
      options->stall_audio = true;
      options->stall_audio_after = atof(v);
    } else if (arg == "--fail-audio-start") {
      const char* v = value();
      if (!v) return false;
      char* end = nullptr;
      const long count = std::strtol(v, &end, 10);
      if (end == v || *end != '\0' || count < 0 || count > 1000) {
        return false;
      }
      options->fail_audio_starts = static_cast<int>(count);
    } else if (arg == "--stall-audio-for") {
      const char* v = value();
      if (!v) return false;
      options->stall_audio_for = atof(v);
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
    } else if (arg == "--live-audio-gap") {
      const char* at = value();
      const char* duration = value();
      if (!at || !duration) return false;
      options->live_audio_gap_at = strtod(at, nullptr);
      options->live_audio_gap_for = strtod(duration, nullptr);
    } else if (arg == "--live-audio-jump") {
      const char* v = value();
      if (!v) return false;
      options->live_audio_jump_at = strtod(v, nullptr);
    } else if (arg == "--audio-only") {
      options->frame_type = 0;
    } else if (arg == "--add-subtitle") {
      const char* v = value();
      if (!v) return false;
      options->add_subtitles.push_back(v);
    } else if (arg == "--subtitle-keep-selection") {
      options->subtitle_keep_selection = true;
    } else if (arg == "--header") {
      const char* v = value();
      if (!v) return false;
      options->headers.append(v).append("\r\n");
    } else if (arg == "--subtitle-encoding") {
      const char* v = value();
      if (!v) return false;
      options->subtitle_encoding = v;
    } else if (arg == "--subtitle-delay") {
      const char* v = value();
      if (!v) return false;
      options->subtitle_delay = strtod(v, nullptr);
    } else if (arg == "--subtitle-ppm") {
      const char* v = value();
      if (!v) return false;
      options->subtitle_ppm = v;
    } else if (arg == "--frame-ppm") {
      // SEC:FILE; the first colon, so FILE may hold one (C:\...).
      const char* v = value();
      if (!v) return false;
      std::string spec = v;
      size_t colon = spec.find(':');
      if (colon == std::string::npos || colon + 1 == spec.size()) return false;
      options->snapshots.push_back(
          {atof(spec.substr(0, colon).c_str()), spec.substr(colon + 1)});
    } else if (arg == "--audio-dump") {
      const char* v = value();
      if (!v) return false;
      options->audio_dump = v;
    } else if (arg == "--pause") {
      const char* at = value();
      const char* duration = value();
      if (!at || !duration) return false;
      options->pause_at = strtod(at, nullptr);
      options->pause_for = strtod(duration, nullptr);
    } else if (arg == "--virtual-time") {
      options->virtual_time = true;
    } else if (arg == "--null-audio") {
      options->null_audio = true;
    } else if (arg == "--verbose") {
      options->verbose = true;
    } else if (arg == "--no-hw") {
      options->hardware = false;
    } else if (arg == "--cpu") {
      options->frame_type = LYCHEE_FRAME_CPU_BGRA;
    } else if (arg == "--rgba") {
      options->frame_type = LYCHEE_FRAME_CPU_RGBA;
    } else if (arg == "--android-surface") {
      // Android: MediaCodec frames, shown in no window (the core's
      // placeholder). Real time only: the codec holds a few frames, short
      // of what --virtual-time waits for, and the player takes it as hung.
      options->frame_type = LYCHEE_FRAME_ANDROID_SURFACE;
    } else if (arg == "--native") {
      options->frame_type = LYCHEE_FRAME_CVPIXELBUFFER_NATIVE;
    } else if (arg == "--d3d11") {
      // CPU frames too, as the Flutter plugin takes them.
      options->frame_type = LYCHEE_FRAME_D3D11_TEXTURE | LYCHEE_FRAME_CPU_BGRA;
    } else if (arg == "--d3d11-hdr") {
      options->frame_type = LYCHEE_FRAME_D3D11_TEXTURE_HDR |
                            LYCHEE_FRAME_D3D11_TEXTURE | LYCHEE_FRAME_CPU_BGRA;
    } else if (arg == "--dmabuf") {
      // Dmabufs on the GPU of a DRM render node, CPU frames too, as the
      // Flutter plugin takes them.
      const char* v = value();
      if (!v) return false;
      options->dmabuf_node = v;
      options->frame_type = LYCHEE_FRAME_DMABUF | LYCHEE_FRAME_CPU_RGBA;
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

// The --header lines, for every URL the CLI opens.
lychee_open_options OpenOptions(const Options& options) {
  lychee_open_options open_options = {};
  open_options.struct_size = sizeof(open_options);
  open_options.headers = options.headers.c_str();
  return open_options;
}

int32_t Open(lychee_player* player,
             const Options& options,
             const std::string& url) {
  const lychee_open_options open_options = OpenOptions(options);
  return lychee_player_open_with_options(player, url.c_str(), &open_options);
}

void PrintTracks(lychee_player* player) {
  int32_t count = lychee_player_get_track_count(player);
  for (int32_t i = 0; i < count; ++i) {
    lychee_track_info info = {};
    info.struct_size = sizeof(info);
    if (lychee_player_get_track_info(player, i, &info) != 0) {
      continue;
    }
    std::string line;
    Appendf(&line, "track %d: %s %s", i, TrackTypeName(info.type), info.codec);
    if (info.type == LYCHEE_TRACK_VIDEO) {
      Appendf(&line, " %dx%d rotation=%d", info.width, info.height,
              lychee_player_get_video_rotation(player));
    } else if (info.type == LYCHEE_TRACK_AUDIO) {
      Appendf(&line, " %d Hz %d ch", info.sample_rate, info.channels);
    }
    Appendf(&line, " lang=%s title=%s selected=%d%s", info.language, info.title,
            info.selected, info.external ? " external" : "");
    // What the container says about it, where it says anything.
    static const char* const kRoles[] = {"main", "commentary", "description",
                                         "hearing-impaired"};
    if (info.is_default) {
      line += " default";
    }
    if (info.forced) {
      line += " forced";
    }
    if (info.role > 0 && info.role < 4) {
      Appendf(&line, " role=%s", kRoles[info.role]);
    }
    if (info.bitrate > 0) {
      Appendf(&line, " bitrate=%lld", static_cast<long long>(info.bitrate));
    }
    printf("%s\n", line.c_str());
  }
}

void PrintVariants(lychee_player* player) {
  const int32_t count = lychee_player_get_variant_count(player);
  for (int32_t i = 0; i < count; ++i) {
    lychee_variant_info info = {};
    info.struct_size = sizeof(info);
    if (lychee_player_get_variant_info(player, i, &info) != 0) {
      continue;
    }
    printf("variant %d: %" PRId64 " bit/s %dx%d fps=%.3f codecs=%s track=%d\n",
           i, info.bandwidth, info.width, info.height, info.frame_rate,
           info.codecs, info.track);
  }
}

// The seekable range and the live edge, as "seekable 0.000-12.000s" and
// " edge 6.000s".
std::string SeekableRange(lychee_player* player) {
  int64_t start = 0;
  int64_t end = 0;
  if (lychee_player_get_seekable_range(player, &start, &end) != 0) {
    return "seekable none";
  }
  char text[96];
  const int64_t edge = lychee_player_get_live_edge(player);
  snprintf(text, sizeof(text), "seekable %.3f-%.3fs", start / 1e6, end / 1e6);
  std::string result = text;
  if (edge >= 0) {
    snprintf(text, sizeof(text), " edge %.3fs", edge / 1e6);
    result += text;
  }
  return result;
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
// Normalized RGB bytes, retaining PQ encoding for native HDR textures.
void TextureRgb(const uint8_t* pixel, DXGI_FORMAT format, int* rgb) {
  if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
    uint32_t value = 0;
    std::memcpy(&value, pixel, sizeof(value));
    for (int i = 0; i < 3; ++i) {
      rgb[i] =
          static_cast<int>(((value >> (10 * i)) & 1023) * 255 + 511) / 1023;
    }
  } else {
    rgb[0] = pixel[2];
    rgb[1] = pixel[1];
    rgb[2] = pixel[0];
  }
}

// The center pixel of a D3D11 texture frame, as R, G, B.
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
  opened->GetDesc(&desc);
  desc.Width = desc.Height = 1;
  desc.MipLevels = desc.ArraySize = 1;
  desc.SampleDesc = {1, 0};
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
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
    TextureRgb(p, desc.Format, rgb);
    if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
      uint32_t value = 0;
      std::memcpy(&value, p, sizeof(value));
      printf(
          "native frame: R10G10B10A2 transfer=16 primaries=9 rgb10=%u,%u,%u\n",
          value & 1023, (value >> 10) & 1023, (value >> 20) & 1023);
    }
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
            static_cast<ptrdiff_t>(y * image.bitmap_height / image.height) *
                image.stride +
            (x * image.bitmap_width / image.width) * 4;
        uint8_t* out =
            canvas.data() + (static_cast<size_t>(cy) * width + cx) * 3;
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

// A frame's pixels as RGB, 3 bytes a pixel, top row first.
struct RgbImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
};

// Copies |height| rows of 4-byte pixels into |image|; |bgra| says which way
// round red and blue are.
void CopyRgb(const uint8_t* data,
             size_t stride,
             int width,
             int height,
             bool bgra,
             RgbImage* image) {
  image->width = width;
  image->height = height;
  image->pixels.resize(static_cast<size_t>(width) * height * 3);
  uint8_t* out = image->pixels.data();
  for (int y = 0; y < height; ++y) {
    const uint8_t* p = data + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x, p += 4, out += 3) {
      out[0] = p[bgra ? 2 : 0];
      out[1] = p[1];
      out[2] = p[bgra ? 0 : 2];
    }
  }
}

#if defined(__APPLE__)
// A CVPixelBuffer frame on the CPU. The decoder's own YCbCr buffers
// (--native) go through VideoToolbox to SDR BT.709 as their color
// attachments say, as a display that honors them would show them.
bool ReadPixelBuffer(CVPixelBufferRef buffer, RgbImage* image) {
  CVPixelBufferRef bgra = nullptr;
  if (CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_32BGRA) {
    bgra = CVPixelBufferRetain(buffer);
  } else {
    VTPixelTransferSessionRef session = nullptr;
    if (VTPixelTransferSessionCreate(nullptr, &session) != noErr) {
      return false;
    }
    VTSessionSetProperty(session,
                         kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                         kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(
        session, kVTPixelTransferPropertyKey_DestinationTransferFunction,
        kCVImageBufferTransferFunction_ITU_R_709_2);
    VTSessionSetProperty(session,
                         kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                         kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    // Tone-mapping HDR needs an IOSurface to write to, as the library's
    // own converter has.
    CFDictionaryRef io_surface = CFDictionaryCreate(
        nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    const void* key = kCVPixelBufferIOSurfacePropertiesKey;
    const void* value = io_surface;
    CFDictionaryRef attributes = CFDictionaryCreate(
        nullptr, &key, &value, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (CVPixelBufferCreate(nullptr, CVPixelBufferGetWidth(buffer),
                            CVPixelBufferGetHeight(buffer),
                            kCVPixelFormatType_32BGRA, attributes,
                            &bgra) != kCVReturnSuccess ||
        VTPixelTransferSessionTransferImage(session, buffer, bgra) != noErr) {
      if (bgra) CVPixelBufferRelease(bgra);
      bgra = nullptr;
    }
    CFRelease(attributes);
    CFRelease(io_surface);
    VTPixelTransferSessionInvalidate(session);
    CFRelease(session);
    if (!bgra) {
      return false;
    }
  }
  CVPixelBufferLockBaseAddress(bgra, kCVPixelBufferLock_ReadOnly);
  CopyRgb(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(bgra)),
          CVPixelBufferGetBytesPerRow(bgra),
          static_cast<int>(CVPixelBufferGetWidth(bgra)),
          static_cast<int>(CVPixelBufferGetHeight(bgra)), true, image);
  CVPixelBufferUnlockBaseAddress(bgra, kCVPixelBufferLock_ReadOnly);
  CVPixelBufferRelease(bgra);
  return true;
}
#endif

#if defined(_WIN32)
// A LYCHEE_FRAME_D3D11_TEXTURE frame, copied to a staging texture on
// |device|. The texture can be larger than the picture.
bool ReadTexture(ID3D11Device* device,
                 ID3D11DeviceContext* context,
                 lychee_frame* frame,
                 RgbImage* image) {
  using Microsoft::WRL::ComPtr;
  auto* texture =
      static_cast<ID3D11Texture2D*>(lychee_frame_get_native_handle(frame));
  ComPtr<IDXGIResource> resource;
  HANDLE shared = nullptr;
  ComPtr<ID3D11Texture2D> opened;
  if (FAILED(texture->QueryInterface(IID_PPV_ARGS(&resource))) ||
      FAILED(resource->GetSharedHandle(&shared)) ||
      FAILED(device->OpenSharedResource(shared, IID_PPV_ARGS(&opened)))) {
    return false;
  }
  D3D11_TEXTURE2D_DESC desc = {};
  opened->GetDesc(&desc);
  desc.MipLevels = desc.ArraySize = 1;
  desc.SampleDesc = {1, 0};
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) {
    return false;
  }
  context->CopyResource(staging.Get(), opened.Get());
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
    return false;
  }
  const int width = std::min<int>(lychee_frame_get_width(frame), desc.Width);
  const int height = std::min<int>(lychee_frame_get_height(frame), desc.Height);
  if (desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
    image->width = width;
    image->height = height;
    image->pixels.resize(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
      const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(y) * mapped.RowPitch;
      for (int x = 0; x < width; ++x) {
        int rgb[3] = {};
        TextureRgb(row + static_cast<size_t>(x) * 4, desc.Format, rgb);
        for (int c = 0; c < 3; ++c) {
          image->pixels[(static_cast<size_t>(y) * width + x) * 3 + c] =
              static_cast<uint8_t>(rgb[c]);
        }
      }
    }
  } else {
    CopyRgb(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, width,
            height, true, image);
  }
  context->Unmap(staging.Get(), 0);
  return true;
}
#endif

// |image| turned clockwise by |degrees| (0, 90, 180, 270), as a display
// would show it.
RgbImage Rotate(const RgbImage& image, int degrees) {
  if (degrees % 360 == 0) {
    return image;
  }
  const int w = image.width, h = image.height;
  RgbImage out;
  const bool quarter = degrees % 180 != 0;
  out.width = quarter ? h : w;
  out.height = quarter ? w : h;
  out.pixels.resize(image.pixels.size());
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int ox = 0, oy = 0;
      switch (degrees % 360) {
        case 90:
          ox = h - 1 - y, oy = x;
          break;
        case 180:
          ox = w - 1 - x, oy = h - 1 - y;
          break;
        default:  // 270
          ox = y, oy = w - 1 - x;
          break;
      }
      memcpy(&out.pixels[(static_cast<size_t>(oy) * out.width + ox) * 3],
             &image.pixels[(static_cast<size_t>(y) * w + x) * 3], 3);
    }
  }
  return out;
}

// Writes |frame| as shown, upright, to a binary PPM and prints the line
// tool/frame_check.py reads.
void WriteFramePpm(lychee_player* player,
                   lychee_frame* frame,
                   const std::string& path,
                   [[maybe_unused]] void* device,
                   [[maybe_unused]] void* context) {
  RgbImage image;
  bool read = false;
  const int32_t type = lychee_frame_get_type(frame);
  if (type == LYCHEE_FRAME_CPU_BGRA || type == LYCHEE_FRAME_CPU_RGBA) {
    int32_t stride = 0;
    const uint8_t* data = lychee_frame_get_data(frame, &stride);
    if (data) {
      CopyRgb(data, stride, lychee_frame_get_width(frame),
              lychee_frame_get_height(frame), type == LYCHEE_FRAME_CPU_BGRA,
              &image);
      read = true;
    }
  }
#if defined(__APPLE__)
  if (type == LYCHEE_FRAME_CVPIXELBUFFER ||
      type == LYCHEE_FRAME_CVPIXELBUFFER_NATIVE) {
    read = ReadPixelBuffer(
        static_cast<CVPixelBufferRef>(lychee_frame_get_native_handle(frame)),
        &image);
  }
#endif
#if defined(_WIN32)
  if (type == LYCHEE_FRAME_D3D11_TEXTURE ||
      type == LYCHEE_FRAME_D3D11_TEXTURE_HDR) {
    read =
        ReadTexture(static_cast<ID3D11Device*>(device),
                    static_cast<ID3D11DeviceContext*>(context), frame, &image);
  }
#endif
  if (!read) {
    printf("snapshot %s failed: frame type %d\n", path.c_str(), type);
    return;
  }
  image = Rotate(image, lychee_player_get_video_rotation(player));
  FILE* file = fopen(path.c_str(), "wb");
  if (!file) {
    printf("snapshot %s failed: cannot write\n", path.c_str());
    return;
  }
  fprintf(file, "P6\n%d %d\n255\n", image.width, image.height);
  fwrite(image.pixels.data(), 1, image.pixels.size(), file);
  fclose(file);
  printf("snapshot %s pts=%" PRId64 " size=%dx%d\n", path.c_str(),
         lychee_frame_get_pts(frame), image.width, image.height);
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
      if (packet_.type == LYCHEE_TRACK_AUDIO && InAudioGap(since)) {
        continue;
      }
      // A sender whose audio timestamps jump 16 hours ahead: more frames
      // of silence before them than an int holds.
      if (packet_.type == LYCHEE_TRACK_AUDIO &&
          options_.live_audio_jump_at >= 0 &&
          since >= static_cast<int64_t>(options_.live_audio_jump_at * 1e6)) {
        packet_.pts_us += int64_t{16} * 3600 * 1000000;
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

  bool InAudioGap(int64_t since) const {
    const int64_t gap_at =
        static_cast<int64_t>(options_.live_audio_gap_at * 1e6);
    const int64_t gap_end =
        gap_at + static_cast<int64_t>(options_.live_audio_gap_for * 1e6);
    return options_.live_audio_gap_at >= 0 && since >= gap_at &&
           since < gap_end;
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

#if defined(__ANDROID__)
namespace {

// MediaCodec (Codec2) hands back output through binder callbacks, which
// need a binder thread pool in the process. Apps have one; a program run
// from adb shell gets one only from what starts it, such as AAudio, so
// with the null audio sink a codec never returned output. Looked up at
// run time: libbinder_ndk exports it from API 29, the CLI targets 26.
void StartBinderThreadPool() {
  void* library = dlopen("libbinder_ndk.so", RTLD_NOW);
  if (!library) {
    return;
  }
  using StartThreadPool = void (*)();
  if (auto start = reinterpret_cast<StartThreadPool>(
          dlsym(library, "ABinderProcess_startThreadPool"))) {
    start();
  }
}

}  // namespace
#endif

int main(int argc, char** argv) {
#if defined(__ANDROID__)
  StartBinderThreadPool();
#endif
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    fprintf(
        stderr,
        "usage: %s [--seconds N] [--seek SEC [--seek-after SEC]] "
        "[--volume 0..1] [--gain DB[:PEAK]] [--fade SEC:VOLUME:MS]... "
        "[--audio-samples SEC:FRAMES]... [--refresh HZ] [--vsync-jitter US] "
        "[--no-hw] "
        "[--cpu | --rgba | --native | --d3d11 | --d3d11-hdr | --dmabuf NODE | "
        "--android-surface] "
        "[--rate R [--rate-after "
        "SEC]] [--loop [--loop-off-after SEC]] "
        "[--open-after SEC URL] [--close-after SEC] [--start SEC] "
        "[--switch-after SEC URL] [--next URL [--clear-next-after SEC]] "
        "[--soak SEC "
        "URL] [--select "
        "TYPE:INDEX[@SEC]]... "
        "[--select-after SEC] [--variant INDEX@SEC]... "
        "[--max-bitrate BPS[@SEC]] [--seek-edge SEC] "
        "[--lose-decoder-after SEC]... "
        "[--hang-decoder-after SEC]... [--rebuild-decoder-after SEC]... "
        "[--stall-audio-after SEC "
        "[--stall-audio-for SEC]] [--fail-audio-start N] "
        "[--hide-after SEC [--hide-for SEC]] [--audio-only] "
        "[--live [--latency MS] [--live-jitter MS] "
        "[--live-stall SEC FOR] [--live-flush SEC] [--live-gap SEC FOR] "
        "[--live-audio-gap SEC FOR] [--live-audio-jump SEC]] "
        "[--add-subtitle URL]... [--subtitle-encoding NAME] "
        "[--subtitle-keep-selection] [--header 'NAME: VALUE']... "
        "[--subtitle-delay SEC] [--subtitle-ppm FILE] [--frame-ppm "
        "SEC:FILE]... [--virtual-time] "
        "[--null-audio] [--verbose] [--audio-dump FILE] [--pause SEC FOR] "
        "<file or url>\n",
        argv[0]);
    return 2;
  }

  lychee_set_log_callback(LogToStdout, nullptr);
  if (options.virtual_time) {
    lychee_testing_enable_virtual_time();
  }
  if (options.verbose) {
    lychee_set_log_level(LYCHEE_LOG_DEBUG);
  }
  if (options.null_audio) {
    lychee_testing_enable_null_audio();
  }
  // Where the recording starts on the lychee_now_us() timeline.
  int64_t capture_start = 0;
  if (!options.audio_dump.empty()) {
    lychee_testing_capture_audio();
    capture_start = lychee_now_us();
  }
  if (options.fail_audio_starts > 0) {
    lychee_testing_fail_audio_starts(options.fail_audio_starts);
  }
  if (options.stall_audio && options.stall_audio_after <= 0) {
    options.stall_audio_after = 0;
    printf("stall audio device at start\n");
    lychee_testing_stall_audio_devices(1);
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
  if (!options.dmabuf_node.empty()) {
    lychee_video_device dmabuf_device = {};
    dmabuf_device.struct_size = sizeof(dmabuf_device);
    dmabuf_device.type = LYCHEE_VIDEO_DEVICE_DRM_RENDER_NODE;
    dmabuf_device.path = options.dmabuf_node.c_str();
    lychee_player_set_video_device(player, &dmabuf_device);
  }
  lychee_player_set_event_callback(player, &OnEvent, nullptr);
  lychee_player_set_volume(player, options.volume);
  lychee_player_set_gain(player, options.gain_db, options.gain_peak);
  if (options.max_bitrate >= 0 && options.max_bitrate_after < 0) {
    lychee_player_set_max_bitrate(player, options.max_bitrate);
  }
  lychee_player_set_looping(player, options.loop ? 1 : 0);
  if (options.rate_after < 0) {
    lychee_player_set_rate(player, options.rate);
  }
  // Wall-clock latencies, which the virtual clock hides: from the open to
  // the first frame pulled, and from the seek to the first frame after it.
  const auto wall_now = [] { return std::chrono::steady_clock::now(); };
  const auto wall_open = wall_now();
  auto wall_seek = wall_open;
  double open_ms = -1;
  double seek_ms = -1;
  std::unique_ptr<LiveFeed> live;
  if (options.live) {
    live = std::make_unique<LiveFeed>(options, player);
    if (!live->Open(options.frame_type != 0)) {
      fprintf(stderr, "cannot read %s\n", options.url.c_str());
      return 1;
    }
  } else if (Open(player, options, options.url) != 0) {
    fprintf(stderr, "open rejected\n");
    return 1;
  }
  if (options.start_at >= 0) {
    printf("start at %.3fs\n", options.start_at);
    lychee_player_seek(player, static_cast<int64_t>(options.start_at * 1e6));
  }
  if (!options.next_url.empty()) {
    const lychee_open_options next_options = OpenOptions(options);
    printf("next -> %s: result=%d\n", options.next_url.c_str(),
           lychee_player_set_next(player, options.next_url.c_str(),
                                  &next_options));
  }
  lychee_player_set_subtitle_delay(
      player, static_cast<int64_t>(options.subtitle_delay * 1e6));
  for (const std::string& subtitle : options.add_subtitles) {
    lychee_subtitle_options subtitle_options = {};
    subtitle_options.struct_size = sizeof(subtitle_options);
    subtitle_options.encoding = options.subtitle_encoding.empty()
                                    ? nullptr
                                    : options.subtitle_encoding.c_str();
    subtitle_options.headers = options.headers.c_str();
    subtitle_options.keep_selection = options.subtitle_keep_selection ? 1 : 0;
    printf("add subtitle %s: %d\n", subtitle.c_str(),
           lychee_player_add_subtitle_with_options(player, subtitle.c_str(),
                                                   &subtitle_options));
  }
  // Start after the early returns above; the only exit below stops and joins
  // the thread before destroying the player. std::jthread is still experimental
  // in the Android NDK's libc++.
  std::atomic<bool> stop_null_audio{false};
  std::thread null_audio_thread;
  if (options.null_audio && !options.virtual_time) {
    // The renderer sees real concurrent device pulls, including a stall and
    // recovery, without requiring the host to have an audio output device.
    null_audio_thread = std::thread([&stop_null_audio] {
      while (!stop_null_audio.load()) {
        lychee_testing_render_audio();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }
  lychee_player_play(player);
  bool reopened = false;
  // --close-after: when the player was closed, 0 before.
  int64_t closed_at = 0;
  // --switch-after: the position at the switch (INT64_MIN before, and
  // once reported), the frames pulled since with an earlier pts, and the
  // times the position was reported earlier.
  bool switched = false;
  bool next_cleared = false;
  int64_t switch_position = INT64_MIN;
  int64_t switch_first_pts = INT64_MIN;
  int64_t switch_early_frames = 0;
  int64_t switch_position_dips = 0;
  if (options.reopen_after == 0) {
    printf("open -> %s\n", options.reopen_url.c_str());
    Open(player, options, options.reopen_url);
    reopened = true;
  }

  const int64_t vsync_interval =
      static_cast<int64_t>(1e6 / options.refresh_rate);
  std::mt19937 random(1);
  std::uniform_int_distribution<int64_t> jitter(-options.vsync_jitter,
                                                options.vsync_jitter);
  const int64_t start = lychee_now_us();
  int64_t next_vsync = start;
  // A call that stops a pipeline (a switch, a close) fades its audio out
  // and waits for the device to play that. Virtual time stands still while
  // this thread is in it, so it runs at the real rate meanwhile, as at the
  // destroy below, and the vsyncs go on from where it got to.
  const auto stopping = [&](auto&& call) {
    if (!options.virtual_time) {
      return call();
    }
    std::atomic<bool> done{false};
    const int64_t from = lychee_now_us();
    const auto real_from = std::chrono::steady_clock::now();
    std::thread clock([&done, from, real_from] {
      while (!done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lychee_testing_set_time(
            from + std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - real_from)
                       .count());
      }
    });
    const auto result = call();
    done = true;
    clock.join();
    next_vsync = lychee_now_us();
    return result;
  };
  int64_t next_report = start + 1000000;
  int64_t last_pts = INT64_MIN;
  int64_t backward_jumps = 0;
  int64_t frames_pulled = 0;
  bool seek_issued = false;
  int64_t seek_issued_at = 0;
  // --soak: when the next action is due (0: once ready), and how many ran.
  int64_t soak_next = 0;
  int64_t soak_actions = 0;
  bool soak_on_alternate = false;
  int64_t first_pts_after_seek = INT64_MIN;
  // Virtual time with the volume up: the position at which the silent
  // device first played sound (testing.h), for a clip's A/V sync.
  int64_t audio_onset = INT64_MIN;
  int64_t ready_wall = 0;
  bool stalled_audio = false;
  bool paused = false;
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
  int item_changes = 0;
  // The selections made so far.
  std::vector<bool> selected(options.selections.size());
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
  std::stable_sort(options.snapshots.begin(), options.snapshots.end(),
                   [](const FrameSnapshot& a, const FrameSnapshot& b) {
                     return a.at < b.at;
                   });
  size_t next_snapshot = 0;
  // The frame on screen, kept while snapshots are due.
  lychee_frame* shown = nullptr;

  // Virtual time only: vsyncs where the player's threads did not catch up
  // within kStallTimeout of real time. After kMaxStalls the pipeline is
  // taken to be stuck, and the run stops. Long enough for a sanitizer build
  // on a CI VM to decode in software from a keyframe to a seek's target
  // (seek-ts-hevc10 took over 2 s under ASan).
  constexpr auto kStallTimeout = std::chrono::seconds(10);
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
        if (frame_type == LYCHEE_FRAME_DMABUF && frames_pulled == 0) {
          const auto* dmabuf = static_cast<const lychee_dmabuf*>(
              lychee_frame_get_native_handle(frame));
          if (dmabuf && dmabuf->struct_size >= sizeof(lychee_dmabuf)) {
            printf("dmabuf: fourcc=%08" PRIx32
                   " primaries=%d transfer=%d matrix=%d full_range=%d\n",
                   dmabuf->fourcc, dmabuf->color_primaries,
                   dmabuf->color_transfer, dmabuf->color_matrix,
                   dmabuf->full_range);
          }
        }
#if defined(__APPLE__)
        if (frame_type == LYCHEE_FRAME_CVPIXELBUFFER_NATIVE &&
            frames_pulled == 0) {
          PrintNativeFrame(frame);
        }
#endif
#if defined(_WIN32)
        if ((frame_type == LYCHEE_FRAME_D3D11_TEXTURE ||
             frame_type == LYCHEE_FRAME_D3D11_TEXTURE_HDR) &&
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
        if (switch_position != INT64_MIN) {
          if (switch_first_pts == INT64_MIN) switch_first_pts = pts;
          if (pts < switch_position) switch_early_frames++;
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
          seek_ms =
              std::chrono::duration<double, std::milli>(wall_now() - wall_seek)
                  .count();
        } else if ((looping || !options.next_url.empty()) &&
                   last_pts != INT64_MIN && pts < last_pts - 1000000) {
          // Looped back to the start, or the next item started.
        } else if (last_pts != INT64_MIN && pts < last_pts) {
          backward_jumps++;
        }
        last_pts = pts;
        if (frames_pulled++ == 0) {
          printf("first frame: pts=%.3fs position=%.3fs\n", pts / 1e6,
                 lychee_player_get_position(player) / 1e6);
          open_ms =
              std::chrono::duration<double, std::milli>(wall_now() - wall_open)
                  .count();
        }
        if (next_snapshot < options.snapshots.size()) {
          // On screen until the next one: a snapshot may be due before.
          if (shown) lychee_frame_release(shown);
          shown = frame;
        } else {
          lychee_frame_release(frame);
        }
      }
    }
    vsyncs_on_screen++;
    steady = steady && playing_steadily;
    while (shown && next_snapshot < options.snapshots.size() &&
           now - start >= static_cast<int64_t>(
                              options.snapshots[next_snapshot].at * 1e6)) {
#if defined(_WIN32)
      WriteFramePpm(player, shown, options.snapshots[next_snapshot].path,
                    device.Get(), context.Get());
#else
      WriteFramePpm(player, shown, options.snapshots[next_snapshot].path,
                    nullptr, nullptr);
#endif
      next_snapshot++;
    }
    if (shown && next_snapshot == options.snapshots.size()) {
      lychee_frame_release(shown);
      shown = nullptr;
    }

    int32_t state = lychee_player_get_state(player);
    int64_t position = lychee_player_get_position(player);
    if (options.virtual_time && audio_onset == INT64_MIN) {
      // Rendered up to the device's latency ahead of now.
      const int64_t sound = lychee_testing_first_sound_us();
      if (sound != INT64_MIN) {
        audio_onset =
            position + static_cast<int64_t>((sound - now) *
                                            lychee_player_get_rate(player));
      }
    }
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
      PrintVariants(player);
      PrintMetadata(player);
      if (lychee_player_is_seekable(player)) {
        printf("%s at %.3fs\n", SeekableRange(player).c_str(), position / 1e6);
      }
    }
    if (const int variant = g_variant_changed.exchange(-1); variant >= 0) {
      lychee_variant_info info = {};
      info.struct_size = sizeof(info);
      lychee_player_get_variant_info(player, variant, &info);
      printf("variant playing: %d (%" PRId64 " bit/s) at %.3fs\n", variant,
             info.bandwidth, position / 1e6);
    }
    for (auto& [index, after] : options.variants) {
      if (after < 0 || ready_wall == 0 ||
          now - ready_wall < static_cast<int64_t>(after * 1e6)) {
        continue;
      }
      after = -1;
      printf("select variant %d at %.3fs: %s\n", index, position / 1e6,
             lychee_player_select_variant(player, index) == 0 ? "accepted"
                                                              : "rejected");
    }
    if (options.max_bitrate_after >= 0 && ready_wall != 0 &&
        now - ready_wall >=
            static_cast<int64_t>(options.max_bitrate_after * 1e6)) {
      options.max_bitrate_after = -1;
      printf("max bitrate %" PRId64 " at %.3fs\n", options.max_bitrate,
             position / 1e6);
      lychee_player_set_max_bitrate(player, options.max_bitrate);
    }
    if (g_subtitle_changed.exchange(false)) {
      std::string text = SubtitleText(player);
      printf("subtitle at %.3fs: %s\n", position / 1e6,
             text.empty() ? "(none)" : text.c_str());
      PrintSubtitleImages(player, options.subtitle_ppm);
    }
    if (const int changes = g_item_changes; changes != item_changes) {
      item_changes = changes;
      if (g_item_gapless) {
        // The position starts over; the drift goes on across the join.
        drift_position -= g_item_duration;
      } else {
        // Switched to: a new timeline, as after an open.
        drift_wall = 0;
        last_pts = INT64_MIN;
        steady = false;
      }
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
    for (size_t i = 0; i < options.selections.size() && ready_wall != 0; ++i) {
      const Selection& selection = options.selections[i];
      const double after =
          selection.after < 0 ? options.select_after : selection.after;
      if (selected[i] || now - ready_wall < static_cast<int64_t>(after * 1e6)) {
        continue;
      }
      selected[i] = true;
      if (select_position == INT64_MIN || tracks_changed) {
        // A new request: "resumed" waits for its own tracks event.
        select_position = position;
        tracks_changed = false;
      }
      int32_t result =
          lychee_player_select_track(player, selection.type, selection.index);
      printf("select %s %d at %.3fs: %s\n", TrackTypeName(selection.type),
             selection.index, position / 1e6,
             result == 0 ? "accepted" : "rejected");
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
        now - ready_wall >=
            static_cast<int64_t>(options.lose_decoder_after[next_loss].after *
                                 1e6)) {
      const bool hang = options.lose_decoder_after[next_loss].hang;
      const bool rebuild = options.lose_decoder_after[next_loss].rebuild;
      next_loss++;
      loss_position = position;
      g_buffering = false;
      printf("%s video decoder at %.3fs\n",
             hang      ? "hang"
             : rebuild ? "rebuild"
                       : "lose",
             position / 1e6);
      if (rebuild) {
        lychee_player_rebuild_video_decoder(player);
      } else if (hang) {
        lychee_testing_hang_video_decoder(player);
      } else {
        lychee_testing_lose_video_decoder(player);
      }
    }
    for (FadeAction& fade : options.fades) {
      if (fade.at >= 0 && ready_wall != 0 &&
          now - ready_wall >= static_cast<int64_t>(fade.at * 1e6)) {
        fade.at = -1;
        printf("fade to %.3f over %lld ms at %.3fs\n", fade.volume,
               static_cast<long long>(fade.duration_us / 1000), position / 1e6);
        lychee_player_fade(player, fade.volume, fade.duration_us);
      }
    }
    for (SamplesPull& pull : options.sample_pulls) {
      if (pull.at < 0 || ready_wall == 0 ||
          now - ready_wall < static_cast<int64_t>(pull.at * 1e6)) {
        continue;
      }
      pull.at = -1;
      // What was heard up to now; on the recording's timeline for
      // tool/audio_check.py --samples.
      std::vector<float> samples(static_cast<size_t>(pull.frames));
      const int32_t got = lychee_player_get_audio_samples(
          player, now, samples.data(), pull.frames);
      std::string line =
          "samples at_us=" + std::to_string(now - capture_start) +
          " frames=" + std::to_string(got) + ":";
      char value[32];
      for (int32_t i = 0; i < got; ++i) {
        snprintf(value, sizeof(value), " %.6g", samples[i]);
        line += value;
      }
      printf("%s\n", line.c_str());
    }
    if (options.pause_at >= 0 && ready_wall != 0) {
      const int64_t since = now - ready_wall;
      if (!paused && since >= static_cast<int64_t>(options.pause_at * 1e6)) {
        paused = true;
        printf("pause at %.3fs\n", position / 1e6);
        lychee_player_pause(player);
      } else if (paused &&
                 since >= static_cast<int64_t>(
                              (options.pause_at + options.pause_for) * 1e6)) {
        options.pause_at = -1;
        printf("play at %.3fs\n", position / 1e6);
        lychee_player_play(player);
      }
    }
    if (options.stall_audio && ready_wall != 0) {
      const int64_t since = now - ready_wall;
      if (!stalled_audio &&
          since >= static_cast<int64_t>(options.stall_audio_after * 1e6)) {
        stalled_audio = true;
        if (options.stall_audio_after > 0) {
          printf("stall audio device at %.3fs\n", position / 1e6);
          lychee_testing_stall_audio_devices(1);
        }
      }
      if (stalled_audio && options.stall_audio_for >= 0 &&
          since >= static_cast<int64_t>(
                       (options.stall_audio_after + options.stall_audio_for) *
                       1e6)) {
        options.stall_audio = false;
        printf("unstall audio device at %.3fs\n", position / 1e6);
        lychee_testing_stall_audio_devices(0);
      }
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
      Open(player, options, options.reopen_url);
      // A new timeline.
      last_pts = INT64_MIN;
      ready_wall = 0;
      steady = false;
      last_state = -1;
      continue;
    }

    if (!next_cleared && options.clear_next_after >= 0 && ready_wall != 0 &&
        now - ready_wall >=
            static_cast<int64_t>(options.clear_next_after * 1e6)) {
      next_cleared = true;
      printf("next cleared at %.3fs: result=%d\n", position / 1e6,
             lychee_player_set_next(player, nullptr, nullptr));
    }
    if (!switched && options.switch_after >= 0 && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.switch_after * 1e6)) {
      switched = true;
      switch_position = position;
      printf("switch at %.3fs -> %s: result=%d state=%s\n", position / 1e6,
             options.switch_url.c_str(), stopping([&] {
               const lychee_open_options switch_options = OpenOptions(options);
               return lychee_player_switch_source(
                   player, options.switch_url.c_str(), 1, &switch_options);
             }),
             StateName(lychee_player_get_state(player)));
    } else if (switch_position != INT64_MIN) {
      if (lychee_player_get_position(player) < switch_position) {
        switch_position_dips++;
      }
      if (state == LYCHEE_STATE_READY) {
        printf(
            "switch from %.3fs: ready at %.3fs, first_pts=%.3fs "
            "early_frames=%" PRId64 " position_dips=%" PRId64 "\n",
            switch_position / 1e6, position / 1e6, switch_first_pts / 1e6,
            switch_early_frames, switch_position_dips);
        switch_position = INT64_MIN;
        drift_wall = now;
        drift_position = position;
      }
    }
    if (options.close_after >= 0 && closed_at == 0 && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.close_after * 1e6)) {
      const int32_t result =
          stopping([&] { return lychee_player_close(player); });
      closed_at = now;
      lychee_frame* frame = lychee_player_acquire_frame(player, display_time);
      printf("close at %.3fs: result=%d state=%s position=%.3fs frame=%d\n",
             position / 1e6, result, StateName(lychee_player_get_state(player)),
             lychee_player_get_position(player) / 1e6, frame ? 1 : 0);
      if (frame) lychee_frame_release(frame);
      last_pts = INT64_MIN;
      ready_wall = 0;
      steady = false;
      last_state = -1;
      continue;
    }
    if (closed_at > 0 && now - closed_at >= 1000000) {
      closed_at = -1;
      printf("open -> %s\n", options.url.c_str());
      lychee_player_open(player, options.url.c_str());
      continue;
    }

    if (options.soak_every > 0 && ready_wall != 0 && now >= soak_next) {
      const bool due = soak_next != 0;
      soak_next = now + static_cast<int64_t>(options.soak_every * 1e6);
      if (due && soak_actions++ % 3 == 2) {
        soak_on_alternate = !soak_on_alternate;
        const std::string& url =
            soak_on_alternate ? options.soak_url : options.url;
        printf("soak: open -> %s\n", url.c_str());
        Open(player, options, url);
        last_pts = INT64_MIN;
        ready_wall = 0;
        soak_next = 0;
        steady = false;
        last_state = -1;
        continue;
      }
      const int64_t duration = lychee_player_get_duration(player);
      if (due && duration > 0) {
        const int64_t target =
            std::uniform_int_distribution<int64_t>(0, duration - 1)(random);
        printf("soak: seek -> %.3fs\n", target / 1e6);
        lychee_player_seek(player, target);
      }
    }

    if (options.seek_to >= 0 && !seek_issued && ready_wall != 0 &&
        now - ready_wall >= static_cast<int64_t>(options.seek_after * 1e6)) {
      printf("seek -> %.3fs\n", options.seek_to);
      lychee_player_seek(player, static_cast<int64_t>(options.seek_to * 1e6));
      seek_issued = true;
      seek_issued_at = now;
      wall_seek = wall_now();
    }
    if (options.seek_edge_after >= 0 && ready_wall != 0 &&
        now - ready_wall >=
            static_cast<int64_t>(options.seek_edge_after * 1e6)) {
      options.seek_edge_after = -1;
      const int64_t edge = lychee_player_get_live_edge(player);
      printf("seek -> edge %.3fs (%s at %.3fs)\n", edge / 1e6,
             SeekableRange(player).c_str(), position / 1e6);
      if (edge >= 0) {
        lychee_player_seek(player, edge);
      }
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
  // One write: other threads print events and log lines meanwhile, and
  // a line built from several writes could take one in its middle.
  std::string summary;
  Appendf(&summary,
          "summary: state=%s position=%.3fs decoder=%s hw=%d pulled=%" PRId64
          " rendered=%" PRId64 " dropped=%" PRId64 " glitches=%" PRId64
          " backward_jumps=%" PRId64
          " frame_type=%d rate=%.2f"
          " max_drift=%.1fms loops=%" PRId64 " stalls=%" PRId64,
          StateName(lychee_player_get_state(player)),
          lychee_player_get_position(player) / 1e6, stats.video_decoder,
          stats.hardware_decoding, frames_pulled, stats.frames_rendered,
          stats.frames_dropped, glitches, backward_jumps, frame_type,
          lychee_player_get_rate(player), max_drift / 1e3, loops, stalls);
  if (seek_issued && first_pts_after_seek != INT64_MIN) {
    Appendf(&summary, " first_pts_after_seek=%.3fs",
            first_pts_after_seek / 1e6);
  }
  if (live) {
    Appendf(&summary,
            " live_vbuf=%.3fs live_abuf=%.3fs live_packets_dropped=%" PRId64
            " live_late=%.3fs live_silence=%.3fs live_behind=%.3fs"
            " audio_decoder=%s",
            live_vbuf / 1e6, live_abuf / 1e6, stats.live_video_packets_dropped,
            stats.live_audio_late_us / 1e6, stats.live_audio_silence_us / 1e6,
            stats.live_audio_behind_us / 1e6, stats.audio_decoder);
  }
  if (audio_onset != INT64_MIN) {
    Appendf(&summary, " audio_onset=%.4fs", audio_onset / 1e6);
  }
  // Last: cases match the fields above in order.
  if (options.soak_every > 0) {
    Appendf(&summary, " soak_actions=%" PRId64, soak_actions);
  }
  if (open_ms >= 0) {
    Appendf(&summary, " open_ms=%.1f", open_ms);
  }
  if (seek_ms >= 0) {
    Appendf(&summary, " seek_ms=%.1f", seek_ms);
  }
  Appendf(&summary, " wall=%.3fs",
          std::chrono::duration<double>(wall_now() - wall_open).count());
  // The library's own playback counts.
  Appendf(&summary,
          " decoded=%" PRId64 " startup=%.3fs rebuffers=%" PRId64
          " rebuffer=%.3fs seeks=%" PRId64
          " last_seek=%.3fs read_video=%" PRId64 "kB read_audio=%" PRId64
          "kB download=%" PRId64
          "kbit/s video_offset=%.1fms audio_output=%dHz/%dch"
          " audio_latency=%.1fms variant=%" PRId64 "kbit/s",
          stats.frames_decoded, stats.startup_time_us / 1e6,
          stats.rebuffer_count, stats.rebuffer_time_us / 1e6, stats.seek_count,
          stats.last_seek_time_us / 1e6, stats.video_bytes_read / 1000,
          stats.audio_bytes_read / 1000,
          stats.download_rate_bps < 0 ? -1 : stats.download_rate_bps / 1000,
          stats.video_offset_us / 1e3, stats.audio_output_sample_rate,
          stats.audio_output_channels, stats.audio_output_latency_us / 1e3,
          stats.variant_bandwidth_bps / 1000);
  summary += '\n';
  fputs(summary.c_str(), stdout);
  fflush(stdout);
  if (center_pixel[0] >= 0) {
    printf("center pixel: %d,%d,%d\n", center_pixel[0], center_pixel[1],
           center_pixel[2]);
  }
  for (; next_snapshot < options.snapshots.size(); ++next_snapshot) {
    printf("snapshot %s failed: no frame by then\n",
           options.snapshots[next_snapshot].path.c_str());
  }
  if (shown) {
    lychee_frame_release(shown);
  }
  const auto destroy_start = std::chrono::steady_clock::now();
  // The device plays on while the player is destroyed: a stop fades out.
  // Virtual time then runs at about the real rate.
  std::atomic<bool> destroyed{false};
  std::thread destroy_clock;
  if (options.virtual_time) {
    destroy_clock = std::thread([&destroyed, destroy_start] {
      // By the real time passed: a sleep lasts ~15 ms on Windows.
      const int64_t from = lychee_now_us();
      while (!destroyed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lychee_testing_set_time(
            from + std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - destroy_start)
                       .count());
      }
    });
  }
  lychee_player_destroy(player);
  const double destroy_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    destroy_start)
          .count();
  destroyed = true;
  if (destroy_clock.joinable()) {
    destroy_clock.join();
    // So the recording ends in the silence after the stop.
    lychee_testing_set_time(lychee_now_us() + 100000);
  }
  if (null_audio_thread.joinable()) {
    stop_null_audio = true;
    null_audio_thread.join();
  }
  printf("destroyed in %.3fs\n", destroy_seconds);
  if (!options.audio_dump.empty() &&
      lychee_testing_write_captured_audio(options.audio_dump.c_str()) != 0) {
    printf("error: cannot write %s\n", options.audio_dump.c_str());
    exit_code = 1;
  }
  return exit_code;
}
