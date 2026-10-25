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

// 2: rate, looping, reopening, tracks, subtitles, metadata, more stats.
// 3: hardware frame types (Android Surface, D3D11, dmabuf) and
//    lychee_player_set_video_output().
#define LYCHEE_API_VERSION 3

typedef struct lychee_player lychee_player;
typedef struct lychee_frame lychee_frame;

// Video frame types. Also used as a bitmask in lychee_config to declare which
// types the caller can consume; the library converts frames as needed.
typedef enum lychee_frame_type {
  // CPU memory, 32-bit BGRA.
  LYCHEE_FRAME_CPU_BGRA = 1 << 0,
  // A kCVPixelFormatType_32BGRA CVPixelBufferRef backed by an IOSurface.
  LYCHEE_FRAME_CVPIXELBUFFER = 1 << 1,
  // CPU memory, 32-bit RGBA: what Flutter's pixel buffer textures take on
  // Linux, Windows and Android.
  LYCHEE_FRAME_CPU_RGBA = 1 << 2,
  // Android: the decoder renders straight into the ANativeWindow passed to
  // lychee_player_set_video_output(). Acquiring the frame is what puts it in
  // the window, timestamped with the display time; the frame itself has no
  // data and no native handle. Frames never acquired are dropped unseen.
  LYCHEE_FRAME_ANDROID_SURFACE = 1 << 3,
  // Windows: the native handle is an ID3D11Texture2D* in
  // DXGI_FORMAT_B8G8R8A8_UNORM, created with D3D11_RESOURCE_MISC_SHARED on
  // the adapter passed to lychee_player_set_video_output(), so other devices
  // can open it through IDXGIResource::GetSharedHandle(). Every frame has
  // its own texture, never written after it is handed out.
  LYCHEE_FRAME_D3D11_TEXTURE = 1 << 4,
  // Linux: the native handle is a const lychee_dmabuf* describing YUV
  // planes in DRM PRIME buffers (e.g. VAAPI surfaces), for import with
  // EGL_EXT_image_dma_buf_import. The descriptor and its file descriptors
  // stay valid until the frame is released; dup() the fds to keep them.
  LYCHEE_FRAME_DMABUF = 1 << 5,
} lychee_frame_type;

// The planes of a LYCHEE_FRAME_DMABUF frame, laid out as
// EGL_EXT_image_dma_buf_import takes them.
typedef struct lychee_dmabuf {
  // sizeof(lychee_dmabuf) of the library that filled it in.
  uint32_t struct_size;
  // DRM fourcc of the whole image: DRM_FORMAT_NV12 ('NV12') for 8-bit,
  // DRM_FORMAT_P010 ('P010') for 10-bit video.
  uint32_t fourcc;
  // Visible size; the buffers may be larger (alignment).
  int32_t width;
  int32_t height;
  int32_t plane_count;
  int32_t fd[4];
  uint32_t offset[4];
  uint32_t pitch[4];
  // DRM format modifier of every plane; DRM_FORMAT_MOD_INVALID
  // (0x00ffffffffffffff) when the exporter did not say.
  uint64_t modifier;
  // ITU-T H.273 matrix coefficients (1 BT.709, 5/6 BT.601, 9 BT.2020) and
  // range, for the YUV to RGB conversion.
  int32_t color_matrix;
  int32_t full_range;
} lychee_dmabuf;

typedef enum lychee_state {
  LYCHEE_STATE_IDLE = 0,
  // Opening the input and filling buffers before the first frame.
  LYCHEE_STATE_LOADING = 1,
  LYCHEE_STATE_READY = 2,
  // Waiting for data after a seek or a stall. A dropped network connection
  // is retried here for about 10 s before the input counts as ended.
  LYCHEE_STATE_BUFFERING = 3,
  // Played to the end.
  LYCHEE_STATE_ENDED = 4,
  // Final until the player is destroyed; seeks are ignored. Reached when
  // the input cannot be opened, a decoder fails, or the input stops before
  // its declared duration (a truncated file, a connection that could not be
  // re-established): what was read plays first, then the state changes
  // here instead of ENDED and the position stays where playback stopped.
  // lychee_player_get_error() says which.
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
  // A lychee_player_select_track() request was carried out; the selected
  // tracks may differ from the request if a decoder rejected one.
  LYCHEE_EVENT_TRACKS_CHANGED = 7,
  // The subtitle text on screen changed. arg0: its length in bytes, 0 if
  // none. Read it with lychee_player_get_subtitle().
  LYCHEE_EVENT_SUBTITLE_CHANGED = 8,
} lychee_event_type;

typedef enum lychee_track_type {
  LYCHEE_TRACK_VIDEO = 1,
  LYCHEE_TRACK_AUDIO = 2,
  // Text subtitles (SRT, WebVTT, mov_text, ASS/SSA as plain text). Bitmap
  // subtitles are listed but cannot be selected.
  LYCHEE_TRACK_SUBTITLE = 3,
} lychee_track_type;

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
  // API version 2. Media read ahead of the playback position.
  int64_t video_buffered_us;
  int64_t audio_buffered_us;
  // Moving average of the time decoding one video packet takes.
  int64_t video_decode_time_us;
} lychee_stats;

typedef struct lychee_track_info {
  // Must be sizeof(lychee_track_info).
  uint32_t struct_size;
  // lychee_track_type.
  int32_t type;
  // The container's stream index.
  int32_t id;
  int32_t selected;
  // NUL-terminated, truncated to fit.
  char codec[32];
  // ISO 639 code from the container, or empty.
  char language[16];
  char title[128];
  // Video: display size (sample aspect ratio applied). Otherwise 0.
  int32_t width;
  int32_t height;
  // Audio. Otherwise 0.
  int32_t sample_rate;
  int32_t channels;
} lychee_track_info;

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

// The platform object hardware-decoded video goes to; NULL to detach it.
// The library takes its own reference.
// - Android: an ANativeWindow* for LYCHEE_FRAME_ANDROID_SURFACE frames.
//   Can change at any time without restarting the decoder; while none is
//   set, frames are decoded into a placeholder and not shown.
// - Windows: the IDXGIAdapter* the caller renders with, so
//   LYCHEE_FRAME_D3D11_TEXTURE frames can be opened there. Takes effect at
//   the next lychee_player_open() or track switch. Without one, hardware
//   decoding uses the default adapter and frames come as CPU frames.
// - Linux: a const char* path of the DRM render node the caller renders
//   with ("/dev/dri/renderD128"), or "" when it does not know but can
//   import dmabufs, for LYCHEE_FRAME_DMABUF frames. The library copies it.
//   Takes effect like the adapter on Windows, and without one VAAPI frames
//   come as CPU frames.
// Ignored elsewhere.
LYCHEE_EXPORT void lychee_player_set_video_output(lychee_player* player,
                                                  void* output);

LYCHEE_EXPORT void lychee_player_set_event_callback(
    lychee_player* player,
    lychee_event_callback callback,
    void* user_data);

// Opens a file path or URL asynchronously. Progress is reported through
// LYCHEE_EVENT_STATE_CHANGED. Returns 0 if the request was accepted.
//
// Calling it again stops what is open (the state returns to IDLE, then
// LOADING) and opens |url| on the same player. The event callback, volume,
// rate, looping and play/pause are kept.
LYCHEE_EXPORT int32_t lychee_player_open(lychee_player* player,
                                         const char* url);

// Played from the start again if the state is LYCHEE_STATE_ENDED.
LYCHEE_EXPORT void lychee_player_play(lychee_player* player);
LYCHEE_EXPORT void lychee_player_pause(lychee_player* player);
LYCHEE_EXPORT void lychee_player_seek(lychee_player* player,
                                      int64_t position_us);
// 0.0 - 1.0.
LYCHEE_EXPORT void lychee_player_set_volume(lychee_player* player,
                                            float volume);
// Playback speed, clamped to 0.25 - 4.0; 1.0 is normal. Audio keeps its
// pitch. Kept across lychee_player_open().
LYCHEE_EXPORT void lychee_player_set_rate(lychee_player* player, double rate);
LYCHEE_EXPORT double lychee_player_get_rate(lychee_player* player);
// Non-zero: at the end, play again from the start instead of reaching
// LYCHEE_STATE_ENDED. The state stays READY through the jump, and no
// LYCHEE_EVENT_SEEK_COMPLETED is sent for it.
LYCHEE_EXPORT void lychee_player_set_looping(lychee_player* player,
                                             int32_t looping);
LYCHEE_EXPORT int32_t lychee_player_is_looping(lychee_player* player);

LYCHEE_EXPORT int32_t lychee_player_get_state(lychee_player* player);
// Whether playback is requested (lychee_player_play() since the last pause).
// Stays non-zero at LYCHEE_STATE_ENDED.
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

// Tracks of the open media: audio, video and subtitle streams, in container
// order. Available from LYCHEE_EVENT_MEDIA_INFO on; 0 before.
LYCHEE_EXPORT int32_t lychee_player_get_track_count(lychee_player* player);
// |info->struct_size| must be set by the caller. Returns 0, or -1 if
// |index| is out of range.
LYCHEE_EXPORT int32_t lychee_player_get_track_info(lychee_player* player,
                                                   int32_t index,
                                                   lychee_track_info* info);
// Switches the |type| track to track |index| (an index for
// lychee_player_get_track_info()), or disables |type| if |index| is -1.
// Playback continues from the current position. Returns 0 if the request was
// accepted; LYCHEE_EVENT_TRACKS_CHANGED follows. Rejected: an index of
// another type, a bitmap subtitle track, disabling both audio and video,
// and any request before the media is loaded.
LYCHEE_EXPORT int32_t lychee_player_select_track(lychee_player* player,
                                                 int32_t type,
                                                 int32_t index);
// Copies the subtitle text showing at |display_time_us| (lychee_now_us()
// timebase; 0 for now) into |buffer| (NUL-terminated UTF-8, lines separated
// by '\n'). Returns the full text length; 0 if no cue is showing.
LYCHEE_EXPORT int32_t lychee_player_get_subtitle(lychee_player* player,
                                                 int64_t display_time_us,
                                                 char* buffer,
                                                 int32_t buffer_size);
// Copies the container tag |key| (case-insensitive: "title", "artist",
// "album", ...) into |buffer|. Returns the full value length, or -1 if the
// media has no such tag.
LYCHEE_EXPORT int32_t lychee_player_get_metadata(lychee_player* player,
                                                 const char* key,
                                                 char* buffer,
                                                 int32_t buffer_size);

// |stats->struct_size| must be set by the caller; fields past it are not
// written, so callers built against version 1 keep working.
LYCHEE_EXPORT void lychee_player_get_stats(lychee_player* player,
                                           lychee_stats* stats);

// Returns non-zero if the frame chosen for |display_time_us| differs from the
// last acquired one, i.e. the caller should redraw.
//
// Call it once per vsync with that vsync's display time, even when no redraw
// follows: frames are chosen per vsync interval (Chromium's cadence-based
// algorithm), and the interval is estimated from these calls. A later
// lychee_player_acquire_frame() within the same interval returns the frame
// chosen here.
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
// LYCHEE_FRAME_CPU_BGRA and LYCHEE_FRAME_CPU_RGBA only.
LYCHEE_EXPORT const uint8_t* lychee_frame_get_data(const lychee_frame* frame,
                                                   int32_t* stride);
// Native handle: CVPixelBufferRef, ID3D11Texture2D* or const lychee_dmabuf*
// (see lychee_frame_type). Not retained: retain it yourself if it must
// outlive the frame.
LYCHEE_EXPORT void* lychee_frame_get_native_handle(const lychee_frame* frame);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LYCHEE_LYCHEE_H_
