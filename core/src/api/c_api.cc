#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "base/log.h"
#include "base/time.h"
#include "lychee/lychee.h"
#include "pipeline/player.h"

using lychee::MediaType;
using lychee::Player;
using lychee::VideoFrame;

struct lychee_player {
  std::unique_ptr<Player> player;
};

struct lychee_frame {
  std::shared_ptr<VideoFrame> frame;
};

static_assert(static_cast<int>(MediaType::kVideo) == LYCHEE_TRACK_VIDEO &&
                  static_cast<int>(MediaType::kAudio) == LYCHEE_TRACK_AUDIO &&
                  static_cast<int>(MediaType::kSubtitle) ==
                      LYCHEE_TRACK_SUBTITLE,
              "lychee_track_type mirrors MediaType");

namespace {

// Copies |text| NUL-terminated into |buffer|, truncated to fit. Returns the
// full length.
int32_t CopyString(const std::string& text, char* buffer, int32_t buffer_size) {
  if (buffer && buffer_size > 0) {
    size_t length = std::min(text.size(), static_cast<size_t>(buffer_size - 1));
    memcpy(buffer, text.data(), length);
    buffer[length] = '\0';
  }
  return static_cast<int32_t>(text.size());
}

}  // namespace

extern "C" {

int32_t lychee_api_version(void) {
  return LYCHEE_API_VERSION;
}

int64_t lychee_now_us(void) {
  return lychee::NowMicros();
}

void lychee_set_log_level(int32_t level) {
  level = std::clamp(level, static_cast<int32_t>(LYCHEE_LOG_ERROR),
                     static_cast<int32_t>(LYCHEE_LOG_VERBOSE));
  lychee::SetLogLevel(static_cast<lychee::LogLevel>(level));
}

void lychee_set_log_callback(lychee_log_callback callback, void* user_data) {
  lychee::SetLogSink(callback, user_data);
}

void lychee_config_init(lychee_config* config) {
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
  config->video_frame_types = LYCHEE_FRAME_CPU_BGRA;
  config->hardware_decoding = 1;
}

lychee_player* lychee_player_create(const lychee_config* config) {
  lychee_config defaults;
  lychee_config_init(&defaults);
  if (config) {
    // Older callers may pass a smaller struct; keep defaults for the rest.
    memcpy(&defaults, config,
           std::min<size_t>(config->struct_size, sizeof(defaults)));
  }
  lychee::PlayerConfig player_config;
  player_config.video_frame_types = defaults.video_frame_types;
  player_config.hardware_decoding = defaults.hardware_decoding != 0;
  auto* player = new lychee_player;
  player->player = std::make_unique<Player>(player_config);
  return player;
}

void lychee_player_destroy(lychee_player* player) {
  delete player;
}

void lychee_player_set_event_callback(lychee_player* player,
                                      lychee_event_callback callback,
                                      void* user_data) {
  player->player->SetEventCallback(callback, user_data);
}

int32_t lychee_player_open(lychee_player* player, const char* url) {
  return url && player->player->Open(url) ? 0 : -1;
}

void lychee_player_play(lychee_player* player) {
  player->player->Play();
}

void lychee_player_pause(lychee_player* player) {
  player->player->Pause();
}

void lychee_player_seek(lychee_player* player, int64_t position_us) {
  player->player->Seek(position_us);
}

void lychee_player_set_volume(lychee_player* player, float volume) {
  player->player->SetVolume(volume);
}

void lychee_player_set_rate(lychee_player* player, double rate) {
  player->player->SetRate(rate);
}

double lychee_player_get_rate(lychee_player* player) {
  return player->player->rate();
}

void lychee_player_set_looping(lychee_player* player, int32_t looping) {
  player->player->SetLooping(looping != 0);
}

int32_t lychee_player_is_looping(lychee_player* player) {
  return player->player->looping() ? 1 : 0;
}

int32_t lychee_player_get_track_count(lychee_player* player) {
  return static_cast<int32_t>(player->player->tracks().size());
}

int32_t lychee_player_get_track_info(lychee_player* player,
                                     int32_t index,
                                     lychee_track_info* info) {
  std::vector<lychee::TrackInfo> tracks = player->player->tracks();
  if (!info || index < 0 || index >= static_cast<int32_t>(tracks.size())) {
    return -1;
  }
  const lychee::TrackInfo& track = tracks[index];
  lychee_track_info result = {};
  result.struct_size = sizeof(result);
  result.type = static_cast<int32_t>(track.type);
  result.id = track.stream_index;
  result.selected = track.selected ? 1 : 0;
  CopyString(track.codec, result.codec, sizeof(result.codec));
  CopyString(track.language, result.language, sizeof(result.language));
  CopyString(track.title, result.title, sizeof(result.title));
  result.width = track.width;
  result.height = track.height;
  result.sample_rate = track.sample_rate;
  result.channels = track.channels;
  memcpy(info, &result, std::min<size_t>(info->struct_size, sizeof(result)));
  return 0;
}

int32_t lychee_player_select_track(lychee_player* player,
                                   int32_t type,
                                   int32_t index) {
  if (type != LYCHEE_TRACK_VIDEO && type != LYCHEE_TRACK_AUDIO &&
      type != LYCHEE_TRACK_SUBTITLE) {
    return -1;
  }
  return player->player->SelectTrack(static_cast<MediaType>(type), index) ? 0
                                                                          : -1;
}

int32_t lychee_player_get_subtitle(lychee_player* player,
                                   int64_t display_time_us,
                                   char* buffer,
                                   int32_t buffer_size) {
  return CopyString(player->player->subtitle(display_time_us), buffer,
                    buffer_size);
}

int32_t lychee_player_get_metadata(lychee_player* player,
                                   const char* key,
                                   char* buffer,
                                   int32_t buffer_size) {
  std::string value;
  if (!key || !player->player->metadata(key, &value)) {
    if (buffer && buffer_size > 0) {
      buffer[0] = '\0';
    }
    return -1;
  }
  return CopyString(value, buffer, buffer_size);
}

int32_t lychee_player_get_state(lychee_player* player) {
  return static_cast<int32_t>(player->player->state());
}

int32_t lychee_player_is_playing(lychee_player* player) {
  return player->player->playing() ? 1 : 0;
}

int64_t lychee_player_get_position(lychee_player* player) {
  return player->player->position();
}

int64_t lychee_player_get_duration(lychee_player* player) {
  int64_t duration = player->player->duration();
  return duration == lychee::kNoTimestamp ? -1 : duration;
}

int64_t lychee_player_get_buffered_position(lychee_player* player) {
  return player->player->buffered_position();
}

int32_t lychee_player_get_video_size(lychee_player* player,
                                     int32_t* width,
                                     int32_t* height) {
  int w = 0;
  int h = 0;
  bool has_video = player->player->video_size(&w, &h);
  if (width) {
    *width = w;
  }
  if (height) {
    *height = h;
  }
  return has_video ? 1 : 0;
}

int32_t lychee_player_get_video_rotation(lychee_player* player) {
  return player->player->video_rotation();
}

int32_t lychee_player_get_error(lychee_player* player,
                                char* buffer,
                                int32_t buffer_size) {
  return CopyString(player->player->error(), buffer, buffer_size);
}

void lychee_player_get_stats(lychee_player* player, lychee_stats* stats) {
  lychee_stats result = {};
  result.struct_size = sizeof(result);
  lychee::PlayerStats player_stats = player->player->stats();
  result.hardware_decoding = player_stats.hardware ? 1 : 0;
  result.frames_rendered = player_stats.frames_rendered;
  result.frames_dropped = player_stats.frames_dropped;
  snprintf(result.video_decoder, sizeof(result.video_decoder), "%s",
           player_stats.video_decoder);
  result.video_buffered_us = player_stats.video_buffered;
  result.audio_buffered_us = player_stats.audio_buffered;
  result.video_decode_time_us = player_stats.video_decode_time;
  memcpy(stats, &result, std::min<size_t>(stats->struct_size, sizeof(result)));
}

int32_t lychee_player_has_new_frame(lychee_player* player,
                                    int64_t display_time_us) {
  return player->player->HasNewFrame(display_time_us) ? 1 : 0;
}

lychee_frame* lychee_player_acquire_frame(lychee_player* player,
                                          int64_t display_time_us) {
  std::shared_ptr<VideoFrame> frame =
      player->player->AcquireFrame(display_time_us);
  if (!frame) {
    return nullptr;
  }
  return new lychee_frame{std::move(frame)};
}

void lychee_frame_release(lychee_frame* frame) {
  delete frame;
}

int32_t lychee_frame_get_type(const lychee_frame* frame) {
  return static_cast<int32_t>(frame->frame->storage);
}

int32_t lychee_frame_get_width(const lychee_frame* frame) {
  return frame->frame->width;
}

int32_t lychee_frame_get_height(const lychee_frame* frame) {
  return frame->frame->height;
}

int64_t lychee_frame_get_pts(const lychee_frame* frame) {
  return frame->frame->pts;
}

const uint8_t* lychee_frame_get_data(const lychee_frame* frame,
                                     int32_t* stride) {
  if (stride) {
    *stride = frame->frame->stride;
  }
  return frame->frame->data;
}

void* lychee_frame_get_native_handle(const lychee_frame* frame) {
  return frame->frame->native_handle;
}

}  // extern "C"
