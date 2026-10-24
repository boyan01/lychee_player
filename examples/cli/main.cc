// Headless player: plays a file while pulling frames at a simulated vsync
// (60 Hz by default), and reports timing problems. Used for regression testing without a
// UI toolkit.

#include <lychee/lychee.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

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
  bool hardware = true;
  bool cpu_frames = false;
};

std::atomic<int> g_seek_completed{0};

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
      break;
    case LYCHEE_EVENT_PLAYING_CHANGED:
      printf("event: playing %" PRId64 "\n", arg0);
      break;
    case LYCHEE_EVENT_MEDIA_INFO:
      printf("event: media info, duration %.3fs\n", arg0 / 1e6);
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
    } else if (arg == "--no-hw") {
      options->hardware = false;
    } else if (arg == "--cpu") {
      options->cpu_frames = true;
    } else if (!arg.empty() && arg[0] != '-') {
      options->url = arg;
    } else {
      return false;
    }
  }
  return !options->url.empty();
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    fprintf(stderr,
            "usage: %s [--seconds N] [--seek SEC [--seek-after SEC]] "
            "[--volume 0..1] [--refresh HZ] [--vsync-jitter US] [--no-hw] "
            "[--cpu] <file or url>\n",
            argv[0]);
    return 2;
  }

  lychee_config config;
  lychee_config_init(&config);
  config.video_frame_types = options.cpu_frames
                                 ? LYCHEE_FRAME_CPU_BGRA
                                 : LYCHEE_FRAME_CVPIXELBUFFER;
  config.hardware_decoding = options.hardware ? 1 : 0;

  lychee_player* player = lychee_player_create(&config);
  lychee_player_set_event_callback(player, &OnEvent, nullptr);
  lychee_player_set_volume(player, options.volume);
  if (lychee_player_open(player, options.url.c_str()) != 0) {
    fprintf(stderr, "open rejected\n");
    return 1;
  }
  lychee_player_play(player);

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
  int32_t last_state = -1;
  // Smoothness: a frame lasting D on a display refreshing every I should stay
  // on screen floor(D/I) or ceil(D/I) vsyncs. Any other count is a glitch (a
  // frame repeated or cut short), even when no frame was dropped.
  int64_t glitches = 0;
  int64_t vsyncs_on_screen = 0;
  // The frame on screen has been shown only while playing steadily.
  bool steady = false;
  int exit_code = 0;

  for (;;) {
    next_vsync += vsync_interval;
    std::this_thread::sleep_for(
        std::chrono::microseconds(next_vsync - lychee_now_us()));
    int64_t now = lychee_now_us();
    // Like a real display link: regular vsync timestamps, independent of how
    // late this thread woke up, give or take the timestamp jitter.
    int64_t display_time = next_vsync + vsync_interval + jitter(random);

    bool playing_steadily = last_state == LYCHEE_STATE_READY;
    if (lychee_player_has_new_frame(player, display_time)) {
      lychee_frame* frame = lychee_player_acquire_frame(player, display_time);
      if (frame) {
        int64_t pts = lychee_frame_get_pts(frame);
        // The first discontinuity after a seek is the seek landing.
        bool discontinuity = last_pts != INT64_MIN &&
                             (pts < last_pts || pts > last_pts + 500000);
        if (steady && !discontinuity && pts > last_pts) {
          double ideal = static_cast<double>(pts - last_pts) / vsync_interval;
          if (vsyncs_on_screen < static_cast<int64_t>(ideal + 0.02) ||
              vsyncs_on_screen > static_cast<int64_t>(ideal - 0.02) + 1) {
            glitches++;
          }
        }
        steady = playing_steadily;
        vsyncs_on_screen = 0;
        if (seek_issued && first_pts_after_seek == INT64_MIN &&
            discontinuity) {
          first_pts_after_seek = pts;
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
    }
    last_state = state;

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
        drift = (position - drift_position) - (now - drift_wall);
      }
      printf(
          "t=%5.1fs state=%-9s pos=%7.3fs buffered=%7.3fs decoder=%s hw=%d "
          "rendered=%" PRId64 " dropped=%" PRId64 " drift=%+.1fms\n",
          (now - start) / 1e6, StateName(state), position / 1e6,
          lychee_player_get_buffered_position(player) / 1e6,
          stats.video_decoder, stats.hardware_decoding, stats.frames_rendered,
          stats.frames_dropped, drift / 1e3);
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
         " backward_jumps=%" PRId64,
         StateName(lychee_player_get_state(player)),
         lychee_player_get_position(player) / 1e6, stats.video_decoder,
         stats.hardware_decoding, frames_pulled, stats.frames_rendered,
         stats.frames_dropped, glitches, backward_jumps);
  if (seek_issued && first_pts_after_seek != INT64_MIN) {
    printf(" first_pts_after_seek=%.3fs", first_pts_after_seek / 1e6);
  }
  printf("\n");
  lychee_player_destroy(player);
  return exit_code;
}
