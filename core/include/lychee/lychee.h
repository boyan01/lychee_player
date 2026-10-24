// lychee: a cross-platform media player library.
//
// This is the only public header. Everything is exposed as a C ABI so the
// library can be used from Dart FFI, Swift, Kotlin/JNI, Rust, C#, Qt, etc.
//
// Threading:
// - All lychee_player_* functions are thread-safe.
// - Callbacks are invoked on internal threads. They must return quickly and
//   must not call lychee_player_destroy().
//
// Time values are in microseconds unless stated otherwise.

#ifndef LYCHEE_LYCHEE_H_
#define LYCHEE_LYCHEE_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(LYCHEE_SHARED) && defined(LYCHEE_IMPLEMENTATION)
#define LYCHEE_EXPORT __declspec(dllexport)
#elif defined(LYCHEE_SHARED)
#define LYCHEE_EXPORT __declspec(dllimport)
#else
#define LYCHEE_EXPORT
#endif
#else
#define LYCHEE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LYCHEE_API_VERSION 1

typedef struct lychee_player lychee_player;
typedef struct lychee_frame lychee_frame;

// Video frame types. Also used as a bitmask in lychee_config to declare which
// types the caller can consume; the library converts frames as needed.
typedef enum lychee_frame_type {
  // CPU memory, 32-bit BGRA.
  LYCHEE_FRAME_CPU_BGRA = 1 << 0,
  // A kCVPixelFormatType_32BGRA CVPixelBufferRef backed by an IOSurface.
  LYCHEE_FRAME_CVPIXELBUFFER = 1 << 1,
} lychee_frame_type;

typedef enum lychee_state {
  LYCHEE_STATE_IDLE = 0,
  LYCHEE_STATE_LOADING = 1,
  LYCHEE_STATE_READY = 2,
  LYCHEE_STATE_BUFFERING = 3,
  LYCHEE_STATE_ENDED = 4,
  LYCHEE_STATE_ERROR = 5,
} lychee_state;

typedef enum lychee_event_type {
  // arg0: new lychee_state.
  LYCHEE_EVENT_STATE_CHANGED = 1,
  // arg0: 1 if playing, 0 if paused.
  LYCHEE_EVENT_PLAYING_CHANGED = 2,
  // Duration, tracks and video size are available. arg0: duration.
  LYCHEE_EVENT_MEDIA_INFO = 3,
  // arg0: display width, arg1: display height.
  LYCHEE_EVENT_VIDEO_SIZE_CHANGED = 4,
  // arg0: position the seek landed on.
  LYCHEE_EVENT_SEEK_COMPLETED = 5,
  // Details are available from lychee_player_get_error().
  LYCHEE_EVENT_ERROR = 6,
} lychee_event_type;

typedef enum lychee_log_level {
  LYCHEE_LOG_ERROR = 0,
  LYCHEE_LOG_WARNING = 1,
  LYCHEE_LOG_INFO = 2,
  LYCHEE_LOG_DEBUG = 3,
  LYCHEE_LOG_VERBOSE = 4,
} lychee_log_level;

// Only plain values are passed so the callback can be bound directly to
// asynchronous FFI callbacks (e.g. Dart's NativeCallable.listener).
typedef void (*lychee_event_callback)(void* user_data,
                                      int32_t type,
                                      int64_t arg0,
                                      int64_t arg1);

typedef void (*lychee_log_callback)(void* user_data,
                                    int32_t level,
                                    const char* message);

typedef struct lychee_config {
  // Must be sizeof(lychee_config). Set by lychee_config_init().
  uint32_t struct_size;
  // Bitmask of lychee_frame_type the caller accepts.
  uint32_t video_frame_types;
  // Non-zero to try hardware decoding first.
  int32_t hardware_decoding;
} lychee_config;

typedef struct lychee_stats {
  // Must be sizeof(lychee_stats).
  uint32_t struct_size;
  int32_t hardware_decoding;
  int64_t frames_rendered;
  int64_t frames_dropped;
  char video_decoder[32];
} lychee_stats;

LYCHEE_EXPORT int32_t lychee_api_version(void);

// Monotonic clock used for display times. On Apple platforms this is
// mach_absolute_time() converted to microseconds.
LYCHEE_EXPORT int64_t lychee_now_us(void);

LYCHEE_EXPORT void lychee_set_log_level(int32_t level);
LYCHEE_EXPORT void lychee_set_log_callback(lychee_log_callback callback,
                                           void* user_data);

LYCHEE_EXPORT void lychee_config_init(lychee_config* config);

LYCHEE_EXPORT lychee_player* lychee_player_create(const lychee_config* config);
LYCHEE_EXPORT void lychee_player_destroy(lychee_player* player);

LYCHEE_EXPORT void lychee_player_set_event_callback(
    lychee_player* player,
    lychee_event_callback callback,
    void* user_data);

// Opens a file path or URL asynchronously. Progress is reported through
// LYCHEE_EVENT_STATE_CHANGED. Returns 0 if the request was accepted.
LYCHEE_EXPORT int32_t lychee_player_open(lychee_player* player,
                                         const char* url);

LYCHEE_EXPORT void lychee_player_play(lychee_player* player);
LYCHEE_EXPORT void lychee_player_pause(lychee_player* player);
LYCHEE_EXPORT void lychee_player_seek(lychee_player* player,
                                      int64_t position_us);
// 0.0 - 1.0.
LYCHEE_EXPORT void lychee_player_set_volume(lychee_player* player,
                                            float volume);

LYCHEE_EXPORT int32_t lychee_player_get_state(lychee_player* player);
LYCHEE_EXPORT int32_t lychee_player_is_playing(lychee_player* player);
LYCHEE_EXPORT int64_t lychee_player_get_position(lychee_player* player);
// Returns -1 if unknown (e.g. live streams).
LYCHEE_EXPORT int64_t lychee_player_get_duration(lychee_player* player);
LYCHEE_EXPORT int64_t lychee_player_get_buffered_position(
    lychee_player* player);
// Display size, with the sample aspect ratio applied and before rotation.
// Returns 0 if there is no video.
LYCHEE_EXPORT int32_t lychee_player_get_video_size(lychee_player* player,
                                                   int32_t* width,
                                                   int32_t* height);
// Clockwise rotation in degrees: 0, 90, 180 or 270.
LYCHEE_EXPORT int32_t lychee_player_get_video_rotation(lychee_player* player);
// Copies the last error message into |buffer| (NUL-terminated). Returns the
// full message length.
LYCHEE_EXPORT int32_t lychee_player_get_error(lychee_player* player,
                                              char* buffer,
                                              int32_t buffer_size);

// |stats->struct_size| must be set by the caller.
LYCHEE_EXPORT void lychee_player_get_stats(lychee_player* player,
                                           lychee_stats* stats);

// Returns non-zero if the frame chosen for |display_time_us| differs from the
// last acquired one, i.e. the caller should redraw. Does not consume frames.
LYCHEE_EXPORT int32_t lychee_player_has_new_frame(lychee_player* player,
                                                  int64_t display_time_us);

// Returns the frame that should be on screen at |display_time_us|
// (lychee_now_us() timebase), or NULL if there is none yet. The frame stays
// valid until lychee_frame_release().
LYCHEE_EXPORT lychee_frame* lychee_player_acquire_frame(
    lychee_player* player,
    int64_t display_time_us);

LYCHEE_EXPORT void lychee_frame_release(lychee_frame* frame);
LYCHEE_EXPORT int32_t lychee_frame_get_type(const lychee_frame* frame);
LYCHEE_EXPORT int32_t lychee_frame_get_width(const lychee_frame* frame);
LYCHEE_EXPORT int32_t lychee_frame_get_height(const lychee_frame* frame);
LYCHEE_EXPORT int64_t lychee_frame_get_pts(const lychee_frame* frame);
// LYCHEE_FRAME_CPU_BGRA only.
LYCHEE_EXPORT const uint8_t* lychee_frame_get_data(const lychee_frame* frame,
                                                   int32_t* stride);
// Native handle (e.g. CVPixelBufferRef). Not retained: retain it yourself if
// it must outlive the frame.
LYCHEE_EXPORT void* lychee_frame_get_native_handle(const lychee_frame* frame);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYCHEE_LYCHEE_H_
