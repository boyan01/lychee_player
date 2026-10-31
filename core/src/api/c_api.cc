#define LOG_TAG "api"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "api/testing.h"
#include "base/log.h"
#include "base/time.h"
#include "lychee/lychee.h"
#include "media/annexb.h"
#include "media/factory.h"
#if defined(__ANDROID__) && !defined(LYCHEE_WITH_FFMPEG)
#include "media/ffmpeg_extension.h"
#endif
#include "media/null_audio_sink.h"
#include "pipeline/audio_renderer.h"
#include "pipeline/player.h"

using lychee::MediaType;
using lychee::Player;
using lychee::VideoFrame;

struct lychee_player {
  // Not a unique_ptr: its reset() clears the pointer before ~Player joins
  // the threads, and an event callback may still call into the player
  // until then.
  Player* player = nullptr;
  ~lychee_player() { delete player; }
};

struct lychee_subtitle {
  std::shared_ptr<const lychee::SubtitleImages> images;
};

struct lychee_frame {
  std::shared_ptr<VideoFrame> frame;
  // On the file's timeline (Player::UnloopedPts()).
  int64_t pts;
};

static_assert(static_cast<int>(lychee::VideoDevice::Type::kD3D11Adapter) ==
                      LYCHEE_VIDEO_DEVICE_D3D11_ADAPTER &&
                  static_cast<int>(lychee::VideoDevice::Type::kDrmRenderNode) ==
                      LYCHEE_VIDEO_DEVICE_DRM_RENDER_NODE,
              "lychee_video_device_type mirrors VideoDevice::Type");

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

// |device| as a VideoDevice; none for null. False for an unsupported type.
bool ToVideoDevice(const lychee_video_device* device,
                   lychee::VideoDevice* result) {
  if (!device) {
    *result = lychee::VideoDevice();
    return true;
  }
  // Older callers may pass a smaller struct; zero the rest.
  lychee_video_device copy = {};
  memcpy(&copy, device,
         std::min<size_t>(device->struct_size, sizeof(copy)));
  return lychee::MakeVideoDevice(copy.type, copy.handle, copy.path, result);
}

// Default lychee_live_config.latency_us.
constexpr int64_t kDefaultLiveLatency = 80 * lychee::kMicrosPerMilli;

// Copies a caller's struct that may be smaller than this build's (an older
// caller) or larger (a newer one) into a zeroed |T|.
template <typename T>
T CopyStruct(const T* source) {
  T copy = {};
  memcpy(&copy, source, std::min<size_t>(source->struct_size, sizeof(copy)));
  return copy;
}

// Copies |result| into a caller's struct, no more than its struct_size, and
// leaves that size as it was: a caller reusing the struct must not have the
// next call write past it.
template <typename T>
void CopyOut(const T& result, T* out) {
  const uint32_t size = out->struct_size;
  memcpy(out, &result, std::min<size_t>(size, sizeof(result)));
  out->struct_size = size;
}

lychee_config DefaultConfig() {
  lychee_config config = {};
  config.struct_size = sizeof(config);
  config.video_frame_types = LYCHEE_FRAME_CPU_BGRA;
  config.hardware_decoding = 1;
  return config;
}

// |track| as a live StreamInfo; kUnknown type for LYCHEE_CODEC_NONE. False
// for a codec of the wrong type.
bool ToLiveStream(const lychee_live_track& track, lychee::StreamInfo* info) {
  using lychee::AudioCodec;
  using lychee::VideoCodec;
  *info = lychee::StreamInfo();
  if (track.codec == LYCHEE_CODEC_NONE) {
    return true;
  }
  if (track.type == LYCHEE_TRACK_VIDEO) {
    if (track.codec != LYCHEE_CODEC_H264 && track.codec != LYCHEE_CODEC_HEVC) {
      return false;
    }
    info->type = MediaType::kVideo;
    info->video.codec = track.codec == LYCHEE_CODEC_H264 ? VideoCodec::kH264
                                                         : VideoCodec::kHEVC;
    info->video.codec_name = lychee::VideoCodecName(info->video.codec);
    info->video.width = std::max(track.width, 0);
    info->video.height = std::max(track.height, 0);
    info->video.low_delay = true;
    return true;
  }
  if (track.codec != LYCHEE_CODEC_AAC && track.codec != LYCHEE_CODEC_ALAC) {
    return false;
  }
  info->type = MediaType::kAudio;
  info->audio.codec =
      track.codec == LYCHEE_CODEC_AAC ? AudioCodec::kAAC : AudioCodec::kALAC;
  info->audio.codec_name = lychee::AudioCodecName(info->audio.codec);
  info->audio.sample_rate = track.sample_rate;
  info->audio.channels = track.channels;
  if (track.config && track.config_size > 0) {
    info->audio.extradata.assign(track.config,
                                 track.config + track.config_size);
  }
  return true;
}

}  // namespace

extern "C" {

int32_t lychee_api_version(void) {
  return LYCHEE_API_VERSION;
}

int64_t lychee_now_us(void) {
  return lychee::NowMicros();
}

int32_t lychee_codec_supported(int32_t codec, int32_t hardware) {
  switch (codec) {
    case LYCHEE_CODEC_H264:
      return lychee::VideoCodecSupported(lychee::VideoCodec::kH264,
                                         hardware != 0);
    case LYCHEE_CODEC_HEVC:
      return lychee::VideoCodecSupported(lychee::VideoCodec::kHEVC,
                                         hardware != 0);
    case LYCHEE_CODEC_AAC:
      return lychee::AudioCodecSupported(lychee::AudioCodec::kAAC);
    case LYCHEE_CODEC_ALAC:
      return lychee::AudioCodecSupported(lychee::AudioCodec::kALAC);
    default:
      return 0;
  }
}

void lychee_set_log_level(int32_t level) {
  level = std::clamp(level, static_cast<int32_t>(LYCHEE_LOG_ERROR),
                     static_cast<int32_t>(LYCHEE_LOG_VERBOSE));
  lychee::SetLogLevel(static_cast<lychee::LogLevel>(level));
}

void lychee_set_log_callback(lychee_log_callback callback, void* user_data) {
  lychee::SetLogSink(callback, user_data);
}

void lychee_config_init(lychee_config* config, uint32_t struct_size) {
  lychee_config defaults = DefaultConfig();
  struct_size = std::min<uint32_t>(struct_size, sizeof(defaults));
  memcpy(config, &defaults, struct_size);
  if (struct_size >= sizeof(config->struct_size)) {
    config->struct_size = struct_size;
  }
}

lychee_player* lychee_player_create(const lychee_config* config) {
#if defined(__ANDROID__) && !defined(LYCHEE_WITH_FFMPEG)
  lychee::AndroidFFmpegExtension().Seal();
#endif
  lychee_config defaults = DefaultConfig();
  if (config) {
    // Older callers may pass a smaller struct; keep defaults for the rest.
    memcpy(&defaults, config,
           std::min<size_t>(config->struct_size, sizeof(defaults)));
  }
  lychee::PlayerConfig player_config;
  player_config.video_frame_types = defaults.video_frame_types;
  player_config.hardware_decoding = defaults.hardware_decoding != 0;
  player_config.rotate_surface_frames = defaults.rotate_surface_frames != 0;
  if (!ToVideoDevice(defaults.video_device, &player_config.video_device)) {
    LOGW("unsupported video device type %d; using none",
         defaults.video_device->type);
  }
  auto* player = new lychee_player;
  player->player = new Player(player_config);
  return player;
}

void lychee_player_destroy(lychee_player* player) {
  delete player;
#if defined(__GLIBC__)
  // glibc gives each new thread its own malloc arena and keeps what is
  // freed there: without this, every player's decoded frames stay resident
  // after it is gone (~40 MB per file switch at 1080p).
  malloc_trim(0);
#endif
}

int32_t lychee_player_set_video_device(lychee_player* player,
                                       const lychee_video_device* device) {
  lychee::VideoDevice video_device;
  if (!ToVideoDevice(device, &video_device)) {
    return -1;
  }
  player->player->SetVideoDevice(std::move(video_device));
  return 0;
}

void lychee_player_rebuild_video_decoder(lychee_player* player) {
  player->player->RebuildVideoDecoder();
}

void lychee_player_set_video_surface(lychee_player* player, void* surface) {
  player->player->SetVideoSurface(surface);
}

void lychee_player_set_event_callback(lychee_player* player,
                                      lychee_event_callback callback,
                                      void* user_data) {
  player->player->SetEventCallback(callback, user_data);
}

void lychee_player_clear_event_callback(lychee_player* player) {
  player->player->SetEventCallback(nullptr, nullptr);
}

int32_t lychee_player_open(lychee_player* player, const char* url) {
  return url && player->player->Open(url) ? 0 : -1;
}

int32_t lychee_player_open_live(lychee_player* player,
                                const lychee_live_config* config) {
  int64_t latency = kDefaultLiveLatency;
  if (config) {
    const lychee_live_config copy = CopyStruct(config);
    if (copy.latency_us > 0) {
      latency = copy.latency_us;
    }
  }
  return player->player->OpenLive(latency) ? 0 : -1;
}

int32_t lychee_player_set_live_track(lychee_player* player,
                                     const lychee_live_track* track) {
  if (!track) {
    return -1;
  }
  const lychee_live_track copy = CopyStruct(track);
  lychee::StreamInfo info;
  if ((copy.type != LYCHEE_TRACK_VIDEO && copy.type != LYCHEE_TRACK_AUDIO) ||
      !ToLiveStream(copy, &info)) {
    return -1;
  }
  return player->player->SetLiveTrack(static_cast<MediaType>(copy.type), info)
             ? 0
             : -1;
}

int32_t lychee_player_push_live_packet(lychee_player* player,
                                       const lychee_live_packet* packet) {
  if (!packet) {
    return -1;
  }
  const lychee_live_packet copy = CopyStruct(packet);
  if (copy.size <= 0) {
    return -1;
  }
  return player->player->PushLivePacket(static_cast<MediaType>(copy.type),
                                        copy.pts_us, copy.data,
                                        static_cast<size_t>(copy.size))
             ? 0
             : -1;
}

int32_t lychee_player_flush_live(lychee_player* player, int32_t type) {
  return player->player->FlushLive(static_cast<MediaType>(type)) ? 0 : -1;
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
  CopyOut(result, info);
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

lychee_subtitle* lychee_player_acquire_subtitle(lychee_player* player,
                                                int64_t display_time_us,
                                                int32_t width,
                                                int32_t height) {
  auto images =
      player->player->subtitle_images(display_time_us, width, height);
  if (!images || images->images.empty()) {
    return nullptr;
  }
  return new lychee_subtitle{std::move(images)};
}

void lychee_subtitle_release(lychee_subtitle* subtitle) {
  delete subtitle;
}

int64_t lychee_subtitle_get_id(const lychee_subtitle* subtitle) {
  return static_cast<int64_t>(subtitle->images->id);
}

int32_t lychee_subtitle_get_image_count(const lychee_subtitle* subtitle) {
  return static_cast<int32_t>(subtitle->images->images.size());
}

int32_t lychee_subtitle_get_image(const lychee_subtitle* subtitle,
                                  int32_t index,
                                  lychee_subtitle_image* image) {
  const auto& images = subtitle->images->images;
  if (!image || index < 0 || index >= static_cast<int32_t>(images.size()) ||
      image->struct_size < sizeof(lychee_subtitle_image)) {
    return -1;
  }
  const lychee::SubtitleImage& source = images[index];
  image->x = source.x;
  image->y = source.y;
  image->width = source.width;
  image->height = source.height;
  image->bitmap_width = source.bitmap->width;
  image->bitmap_height = source.bitmap->height;
  image->stride = source.bitmap->width * 4;
  image->data = source.bitmap->rgba.data();
  return 0;
}

int32_t lychee_player_get_artwork(lychee_player* player,
                                  uint8_t* buffer,
                                  int32_t buffer_size) {
  const auto artwork = player->player->artwork();
  if (!artwork) {
    return -1;
  }
  const int32_t size = static_cast<int32_t>(artwork->size());
  if (buffer && buffer_size > 0) {
    std::memcpy(buffer, artwork->data(), std::min(size, buffer_size));
  }
  return size;
}

int32_t lychee_player_is_seekable(lychee_player* player) {
  return player->player->seekable() ? 1 : 0;
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
  lychee::PlayerStats player_stats = player->player->stats();
  result.hardware_decoding = player_stats.hardware ? 1 : 0;
  result.frames_rendered = player_stats.frames_rendered;
  result.frames_dropped = player_stats.frames_dropped;
  snprintf(result.video_decoder, sizeof(result.video_decoder), "%s",
           player_stats.video_decoder);
  result.video_buffered_us = player_stats.video_buffered;
  result.audio_buffered_us = player_stats.audio_buffered;
  result.video_decode_time_us = player_stats.video_decode_time;
  snprintf(result.audio_decoder, sizeof(result.audio_decoder), "%s",
           player_stats.audio_decoder);
  result.live_video_packets_dropped = player_stats.live_video_packets_dropped;
  result.live_audio_late_us = player_stats.live_audio_late;
  result.live_audio_silence_us = player_stats.live_audio_silence;
  result.live_audio_behind_us = player_stats.live_audio_behind;
  result.frames_decoded = player_stats.frames_decoded;
  result.video_bytes_read = player_stats.video_bytes_read;
  result.audio_bytes_read = player_stats.audio_bytes_read;
  result.download_rate_bps = player_stats.download_rate;
  result.startup_time_us = player_stats.startup_time;
  result.rebuffer_count = player_stats.rebuffer_count;
  result.rebuffer_time_us = player_stats.rebuffer_time;
  result.seek_count = player_stats.seek_count;
  result.last_seek_time_us = player_stats.last_seek_time;
  CopyOut(result, stats);
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
  const int64_t pts = player->player->UnloopedPts(frame->pts);
  return new lychee_frame{std::move(frame), pts};
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
  return frame->pts;
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

void lychee_testing_enable_virtual_time(void) {
  lychee::EnableVirtualTime();
}

void lychee_testing_enable_null_audio(void) {
  lychee::UseNullAudioSinkForTesting();
}

void lychee_testing_render_audio(void) {
  lychee::NullAudioSink::RenderAll(lychee::NowMicros());
}

void lychee_testing_set_time(int64_t now_us) {
  lychee::SetVirtualTime(now_us);
  lychee_testing_render_audio();
}

int32_t lychee_testing_player_settled(lychee_player* player,
                                      int64_t until_us) {
  return player->player->SettledForTesting(until_us) ? 1 : 0;
}

void lychee_testing_lose_video_decoder(lychee_player* player) {
  player->player->LoseVideoDecoderForTesting();
}

void lychee_testing_hang_video_decoder(lychee_player* player) {
  player->player->HangVideoDecoderForTesting();
}

void lychee_testing_stall_audio_devices(int32_t stalled) {
  lychee::AudioRenderer::StallDevicesForTesting(stalled != 0);
}

void lychee_testing_fail_audio_starts(int32_t count) {
  lychee::AudioRenderer::FailSinkStartsForTesting(count);
}

int64_t lychee_testing_first_sound_us(void) {
  const int64_t time = lychee::NullAudioSink::FirstSound();
  return time == lychee::kNoTimestamp ? INT64_MIN : time;
}

void lychee_testing_capture_audio(void) {
  lychee::NullAudioSink::StartCapture();
}

int32_t lychee_testing_write_captured_audio(const char* wav_path) {
  return wav_path && lychee::NullAudioSink::WriteCapture(wav_path) ? 0 : -1;
}

}  // extern "C"

struct lychee_testing_live_source {
  std::unique_ptr<lychee::Demuxer> demuxer;
  // Per lychee_track_type: the stream, or -1.
  int stream[3] = {-1, -1, -1};
  lychee::AnnexBConverter converter;
  std::vector<uint8_t> audio_config;
  std::vector<uint8_t> data;
};

extern "C" {

lychee_testing_live_source* lychee_testing_live_source_open(const char* url) {
  auto source = std::make_unique<lychee_testing_live_source>();
  source->demuxer = lychee::CreateDemuxer();
  std::string error;
  if (!source->demuxer || !url || !source->demuxer->Open(url, &error)) {
    return nullptr;
  }
  const lychee::MediaInfo& info = source->demuxer->media_info();
  std::vector<int> streams;
  if (info.default_video_stream >= 0) {
    const lychee::VideoConfig& video =
        info.streams[info.default_video_stream].video;
    if (source->converter.Initialize(video)) {
      source->stream[LYCHEE_TRACK_VIDEO] = info.default_video_stream;
      streams.push_back(info.default_video_stream);
    }
  }
  if (info.default_audio_stream >= 0) {
    const lychee::AudioConfig& audio =
        info.streams[info.default_audio_stream].audio;
    if (audio.codec == lychee::AudioCodec::kAAC ||
        audio.codec == lychee::AudioCodec::kALAC) {
      source->stream[LYCHEE_TRACK_AUDIO] = info.default_audio_stream;
      source->audio_config = audio.extradata;
      streams.push_back(info.default_audio_stream);
    }
  }
  source->demuxer->SetEnabledStreams(streams);
  return source.release();
}

int32_t lychee_testing_live_source_track(lychee_testing_live_source* source,
                                         int32_t type,
                                         lychee_live_track* track) {
  if ((type != LYCHEE_TRACK_VIDEO && type != LYCHEE_TRACK_AUDIO) ||
      source->stream[type] < 0) {
    return -1;
  }
  const lychee::StreamInfo& info =
      source->demuxer->media_info().streams[source->stream[type]];
  lychee_live_track result = {};
  result.type = type;
  if (type == LYCHEE_TRACK_VIDEO) {
    result.codec = info.video.codec == lychee::VideoCodec::kHEVC
                       ? LYCHEE_CODEC_HEVC
                       : LYCHEE_CODEC_H264;
    result.width = info.video.width;
    result.height = info.video.height;
  } else {
    result.codec = info.audio.codec == lychee::AudioCodec::kAAC
                       ? LYCHEE_CODEC_AAC
                       : LYCHEE_CODEC_ALAC;
    result.sample_rate = info.audio.sample_rate;
    result.channels = info.audio.channels;
    result.config = source->audio_config.data();
    result.config_size = static_cast<int32_t>(source->audio_config.size());
  }
  CopyOut(result, track);
  return 0;
}

int32_t lychee_testing_live_source_read(lychee_testing_live_source* source,
                                        lychee_live_packet* packet,
                                        int64_t* send_time_us) {
  for (;;) {
    lychee::EncodedPacket read;
    if (source->demuxer->Read(&read) != lychee::DemuxerReadResult::kOk) {
      return -1;
    }
    if (!read.buffer || read.pts == lychee::kNoTimestamp) {
      continue;
    }
    int32_t type = 0;
    source->data.clear();
    if (read.stream_index == source->stream[LYCHEE_TRACK_VIDEO]) {
      type = LYCHEE_TRACK_VIDEO;
      if (!source->converter.Convert(read.buffer->data(), read.size(),
                                     read.keyframe, &source->data)) {
        continue;
      }
    } else if (read.stream_index == source->stream[LYCHEE_TRACK_AUDIO]) {
      type = LYCHEE_TRACK_AUDIO;
      source->data.assign(read.buffer->data(),
                          read.buffer->data() + read.size());
    } else {
      continue;
    }
    packet->type = type;
    packet->pts_us = read.pts;
    packet->data = source->data.data();
    packet->size = static_cast<int32_t>(source->data.size());
    *send_time_us = read.dts != lychee::kNoTimestamp ? read.dts : read.pts;
    return 0;
  }
}

void lychee_testing_live_source_close(lychee_testing_live_source* source) {
  delete source;
}

}  // extern "C"
