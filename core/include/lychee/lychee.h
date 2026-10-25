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
// 4: lychee_player_set_video_output() split into lychee_video_device
//    (lychee_config, lychee_player_set_video_device(), changeable during
//    playback) and lychee_player_set_video_surface(); video_frame_types 0
//    for audio only.
// 5: live input: lychee_player_open_live() and the packets the caller
//    pushes.
// 6: lychee_config.rotate_surface_frames, for Android windows that cannot
//    rotate (SurfaceView).
// 7: lychee_player_get_artwork(); subtitle pictures (bitmap subtitles,
//    styled ASS): lychee_player_acquire_subtitle() and arg1 of
//    LYCHEE_EVENT_SUBTITLE_CHANGED.
#define LYCHEE_API_VERSION 7

typedef struct lychee_player lychee_player;
typedef struct lychee_frame lychee_frame;
typedef struct lychee_subtitle lychee_subtitle;

// Video frame types. Also used as a bitmask in lychee_config to declare which
// types the caller can consume; the library converts frames as needed. The
// type can change between frames (hardware decoding falling back to
// software, a new video device): check it on every frame.
typedef enum lychee_frame_type {
  // CPU memory, 32-bit BGRA.
  LYCHEE_FRAME_CPU_BGRA = 1 << 0,
  // A kCVPixelFormatType_32BGRA CVPixelBufferRef backed by an IOSurface.
  LYCHEE_FRAME_CVPIXELBUFFER = 1 << 1,
  // CPU memory, 32-bit RGBA: what Flutter's pixel buffer textures take on
  // Linux, Windows and Android.
  LYCHEE_FRAME_CPU_RGBA = 1 << 2,
  // Android: the decoder renders straight into the ANativeWindow passed to
  // lychee_player_set_video_surface(). Acquiring the frame is what puts it in
  // the window, timestamped with the display time; the frame itself has no
  // data and no native handle. Frames never acquired are dropped unseen.
  LYCHEE_FRAME_ANDROID_SURFACE = 1 << 3,
  // Windows: the native handle is an ID3D11Texture2D* in
  // DXGI_FORMAT_B8G8R8A8_UNORM, created with D3D11_RESOURCE_MISC_SHARED on
  // the LYCHEE_VIDEO_DEVICE_D3D11_ADAPTER video device, so other devices
  // can open it through IDXGIResource::GetSharedHandle(). Every frame has
  // its own texture, never written after it is handed out.
  LYCHEE_FRAME_D3D11_TEXTURE = 1 << 4,
  // Linux: the native handle is a const lychee_dmabuf* describing YUV
  // planes in DRM PRIME buffers (e.g. VAAPI surfaces), for import with
  // EGL_EXT_image_dma_buf_import. The descriptor and its file descriptors
  // stay valid until the frame is released; dup() the fds to keep them.
  LYCHEE_FRAME_DMABUF = 1 << 5,
} lychee_frame_type;

// The GPU the caller renders with. Frames that stay on the GPU (D3D11
// textures, dmabufs) are produced only for a device the caller named, since
// only then can it open them; without one, hardware decoding still runs but
// its frames come as CPU frames. Apple platforms need none: IOSurfaces open
// on any GPU.
typedef enum lychee_video_device_type {
  LYCHEE_VIDEO_DEVICE_NONE = 0,
  // Windows: |handle| is an IDXGIAdapter*, for LYCHEE_FRAME_D3D11_TEXTURE
  // frames. The library takes its own reference.
  LYCHEE_VIDEO_DEVICE_D3D11_ADAPTER = 1,
  // Linux: |path| is the DRM render node ("/dev/dri/renderD128"), or ""
  // when the caller does not know it but can import dmabufs, for
  // LYCHEE_FRAME_DMABUF frames. The library copies it.
  LYCHEE_VIDEO_DEVICE_DRM_RENDER_NODE = 2,
} lychee_video_device_type;

typedef struct lychee_video_device {
  // Must be sizeof(lychee_video_device).
  uint32_t struct_size;
  // lychee_video_device_type.
  int32_t type;
  void* handle;
  const char* path;
} lychee_video_device;

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
  // The subtitle on screen changed. arg0: the length in bytes of its text,
  // 0 if none; read it with lychee_player_get_subtitle(). arg1: 1 if it
  // has pictures (lychee_player_acquire_subtitle()), which then hold all
  // of it, text included: draw them instead of the text.
  LYCHEE_EVENT_SUBTITLE_CHANGED = 8,
} lychee_event_type;

typedef enum lychee_track_type {
  LYCHEE_TRACK_VIDEO = 1,
  LYCHEE_TRACK_AUDIO = 2,
  // Text subtitles (SRT, WebVTT, mov_text, ASS/SSA) and bitmap subtitles
  // (PGS, DVB, VobSub).
  LYCHEE_TRACK_SUBTITLE = 3,
} lychee_track_type;

// Codecs of a live input's tracks.
typedef enum lychee_codec {
  LYCHEE_CODEC_NONE = 0,
  // Annex B access units, one picture per packet. Parameter sets (SPS,
  // PPS; VPS for HEVC) travel in band, before a keyframe, at least when
  // they change; the library repeats the last ones before later keyframes
  // that lack them.
  LYCHEE_CODEC_H264 = 1,
  LYCHEE_CODEC_HEVC = 2,
  // Raw access units (no ADTS). |config|: the AudioSpecificConfig, which
  // also says the object type (AAC-LC, AAC-ELD, ...).
  LYCHEE_CODEC_AAC = 3,
  // |config|: the ALAC magic cookie (ALACSpecificConfig).
  LYCHEE_CODEC_ALAC = 4,
} lychee_codec;

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
  // Bitmask of lychee_frame_type the caller accepts. 0 plays audio only:
  // video streams are listed as tracks but never read, decoded or
  // selectable.
  uint32_t video_frame_types;
  // Non-zero to try hardware decoding first.
  int32_t hardware_decoding;
  // API version 4. The video device to start with; copied, so it need not
  // outlive lychee_player_create(). NULL for none.
  const lychee_video_device* video_device;
  // API version 6. Android: non-zero to have MediaCodec render
  // LYCHEE_FRAME_ANDROID_SURFACE frames upright, rotated by
  // lychee_player_get_video_rotation(), for windows that cannot rotate what
  // they show (a SurfaceView before API 33). The frames' width and height
  // stay the unrotated size, and other frame types are never rotated.
  int32_t rotate_surface_frames;
} lychee_config;

typedef struct lychee_live_config {
  // Must be sizeof(lychee_live_config).
  uint32_t struct_size;
  // How long after it arrives a packet is presented, at least: the margin
  // for network jitter and decoding. 0 for 80 ms. Video with B-frames
  // needs more: a frame decodes only once the frames it waits for arrived.
  int64_t latency_us;
} lychee_live_config;

typedef struct lychee_live_track {
  // Must be sizeof(lychee_live_track).
  uint32_t struct_size;
  // LYCHEE_TRACK_VIDEO or LYCHEE_TRACK_AUDIO.
  int32_t type;
  // lychee_codec; LYCHEE_CODEC_NONE removes the track.
  int32_t codec;
  // Video: the coded size if known, else 0; the stream's SPS decides.
  int32_t width;
  int32_t height;
  // Audio.
  int32_t sample_rate;
  int32_t channels;
  // Codec configuration (see lychee_codec); copied.
  const uint8_t* config;
  int32_t config_size;
} lychee_live_track;

typedef struct lychee_live_packet {
  // Must be sizeof(lychee_live_packet).
  uint32_t struct_size;
  // LYCHEE_TRACK_VIDEO or LYCHEE_TRACK_AUDIO.
  int32_t type;
  // On the sender's clock, in microseconds, the same clock for audio and
  // video; any origin.
  int64_t pts_us;
  // Copied.
  const uint8_t* data;
  int32_t size;
} lychee_live_packet;

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

// Changes the video device (see lychee_video_device_type); NULL for none.
// Callable at any time: during playback the video decoder is rebuilt on the
// new device and playback continues from the current position, after a
// short rebuffer. Once this returns, no frame from the old device is handed
// out; frames already acquired stay valid until released. Returns 0, or -1
// if this build does not support |device->type|.
//
// The library also rebuilds the decoder by itself when a hardware decoder
// fails during playback, which is how a lost device (a GPU reset, a driver
// update, MediaCodec reclaiming its codec) shows: first on the same device,
// then, if that fails too, in software.
LYCHEE_EXPORT int32_t lychee_player_set_video_device(
    lychee_player* player,
    const lychee_video_device* device);

// Android: the ANativeWindow* LYCHEE_FRAME_ANDROID_SURFACE frames are
// rendered into, or NULL to detach it. The library takes its own
// reference. Can change at any time without restarting the decoder; while
// none is set, frames are decoded into a placeholder and not shown.
// Ignored elsewhere.
LYCHEE_EXPORT void lychee_player_set_video_surface(lychee_player* player,
                                                   void* surface);

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

// Stops what is open, like lychee_player_open(), and starts a live input:
// packets pushed with lychee_player_push_live_packet() play as they
// arrive, |config->latency_us| after the first one. Returns 0 if accepted.
//
// The library maps the sender's timestamps to presentation times: it
// anchors on the first packet, then slowly steers so the earliest packets
// arrive the latency ahead of their time, which absorbs a slow first
// packet and the drift between the two clocks. Audio and video play when
// due: late video frames are dropped (lychee_stats.frames_dropped), late
// audio is cut and gaps become silence. If the audio device needs more
// lead than the latency leaves, audio plays up to 500 ms behind video
// rather than not at all.
//
// The state goes LOADING -> READY once the first frame or audio is
// decoded, and is never BUFFERING or ENDED. Play, pause, seek, rate and
// track selection do not apply. The position counts from the open, the
// duration is -1, and lychee_player_get_track_info() lists the declared
// tracks. lychee_frame_get_pts() is the frame's presentation time in the
// lychee_now_us() timebase.
LYCHEE_EXPORT int32_t lychee_player_open_live(
    lychee_player* player,
    const lychee_live_config* config);

// Declares, changes or removes (LYCHEE_CODEC_NONE) the codec of a live
// track. Packets pushed after this call belong to the new codec; the
// track's decoder is replaced and starts at the next keyframe. Returns 0,
// or -1 if no live input is open or |track| is invalid.
LYCHEE_EXPORT int32_t lychee_player_set_live_track(
    lychee_player* player,
    const lychee_live_track* track);

// Queues one packet of a declared live track; any thread. Video packets
// before the track's first keyframe (after opening, a track change or a
// flush) are dropped, and so is everything queued if the decoders fall
// over 16 MB behind. Returns 0, or -1 if no live input is open or the
// track has no codec.
LYCHEE_EXPORT int32_t lychee_player_push_live_packet(
    lychee_player* player,
    const lychee_live_packet* packet);

// Drops what was pushed of the |type| track (LYCHEE_TRACK_VIDEO or
// LYCHEE_TRACK_AUDIO; 0 for both) and not played yet, e.g. when the sender
// seeks or the connection resets. Flushing both also anchors the timeline
// again on the next packet. Returns 0, or -1 if no live input is open.
LYCHEE_EXPORT int32_t lychee_player_flush_live(lychee_player* player,
                                               int32_t type);

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
// Playback continues from the current position. Subtitle tracks switch at
// once, with the cue on screen if one is: every subtitle stream is read
// all along. Returns 0 if the request was accepted;
// LYCHEE_EVENT_TRACKS_CHANGED follows. Rejected: an index of
// another type, a subtitle track no decoder takes, disabling both audio
// and video, a video track on a player created for audio only, and any
// request before the media is loaded.
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

// A subtitle picture.
typedef struct lychee_subtitle_image {
  // Must be sizeof(lychee_subtitle_image).
  uint32_t struct_size;
  // Where to draw it, in pixels of the canvas passed to
  // lychee_player_acquire_subtitle().
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
  // Its pixels: |bitmap_width| x |bitmap_height|, premultiplied RGBA (bytes
  // R, G, B, A), |stride| bytes a row; scale them to |width| x |height|.
  int32_t bitmap_width;
  int32_t bitmap_height;
  int32_t stride;
  const uint8_t* data;
} lychee_subtitle_image;

// API version 7. The pictures of the subtitle showing at |display_time_us|
// (lychee_now_us() timebase; 0 for now): bitmap subtitles (PGS, DVB,
// VobSub) and, in builds with libass, styled ASS/SSA with its fonts and
// positions. |width| x |height| is the canvas, the rectangle the video
// picture is drawn in, in pixels (0, 0 for the video's size); pictures are
// placed on it and styled text is rendered at its resolution. Returns NULL
// if the subtitle showing has no pictures (plain text, or none).
//
// Call it for every frame you draw while the last
// LYCHEE_EVENT_SUBTITLE_CHANGED had arg1 1: ASS animates between events.
// While nothing changes it is cheap, and lychee_subtitle_get_id() stays
// the same, so redraw only when the id does. Any thread; the result stays
// valid until lychee_subtitle_release().
LYCHEE_EXPORT lychee_subtitle* lychee_player_acquire_subtitle(
    lychee_player* player,
    int64_t display_time_us,
    int32_t width,
    int32_t height);
LYCHEE_EXPORT void lychee_subtitle_release(lychee_subtitle* subtitle);
// The same for the same pictures, different otherwise.
LYCHEE_EXPORT int64_t lychee_subtitle_get_id(const lychee_subtitle* subtitle);
LYCHEE_EXPORT int32_t lychee_subtitle_get_image_count(
    const lychee_subtitle* subtitle);
// |image->struct_size| must be set by the caller. Returns 0, or -1 if
// |index| is out of range.
LYCHEE_EXPORT int32_t lychee_subtitle_get_image(const lychee_subtitle* subtitle,
                                                int32_t index,
                                                lychee_subtitle_image* image);

// API version 7. Copies the media's cover picture (an ID3 or FLAC
// picture, MP4 cover art, a Matroska image attachment) into |buffer| as the
// container stores it, usually JPEG or PNG: decode it with the platform's
// image decoder. Returns its full size in bytes, or -1 if the media has
// none. Available from LYCHEE_EVENT_MEDIA_INFO on.
LYCHEE_EXPORT int32_t lychee_player_get_artwork(lychee_player* player,
                                                uint8_t* buffer,
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
