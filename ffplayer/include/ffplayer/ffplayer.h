// ffplayer.h: 标准系统包含文件的包含文件
// 或项目特定的包含文件。
#ifndef FFPLAYER_FFPLAYER_H_
#define FFPLAYER_FFPLAYER_H_

#include <thread>
#include <mutex>

#include "../../ffp_msg_queue.h"
#include "../../ffp_packet_queue.h"
#include "../../ffp_decoder.h"
#include "../../ffp_frame_queue.h"
#include "../../ffp_video_render.h"
#include "../../ffp_clock.h"
#include "../../ffp_data_source.h"
#include "../../ffp_audio_render.h"
#include "proto.h"

extern "C" {
#include "SDL2/SDL.h"
#include "libavcodec/avfft.h"
#include "libavdevice/avdevice.h"
#include "libavformat/avformat.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/bprint.h"
#include "libavutil/dict.h"
#include "libavutil/eval.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "libavutil/pixdesc.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
}

#ifdef _FLUTTER

#include "third_party/dart/dart_api_dl.h"

#endif

#ifdef _WIN32
#define FFPLAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define FFPLAYER_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define CACHE_THRESHOLD_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1


/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;

typedef enum ffplayer_info_type {
    buffered
} ffplayer_info_t;


typedef enum FFPlayerState_ {
    FFP_STATE_IDLE = 0,
    FFP_STATE_READY,
    FFP_STATE_BUFFERING,
    FFP_STATE_END
} FFPlayerState;


enum ShowMode {
    SHOW_MODE_NONE = -1,
    SHOW_MODE_VIDEO = 0,
    SHOW_MODE_WAVES,
    SHOW_MODE_RDFT,
    SHOW_MODE_NB
};

typedef struct VideoState {
    SDL_Thread *read_tid;
    AVInputFormat *iformat;
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    AVFormatContext *ic;
    int realtime;

    int audio_stream;

    int av_sync_type;

    double audio_clock;
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    AVStream *audio_st;
    PacketQueue audioq;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    int audio_volume;
    int muted;
    struct AudioParams audio_src;
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;

    ShowMode show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;

    int subtitle_stream;
    AVStream *subtitle_st;
    PacketQueue subtitleq;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    int video_stream;
    AVStream *video_st;
    PacketQueue videoq;
    double max_frame_duration;  // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    int eof;

    char *filename;
    int step;

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    SDL_cond *continue_read_thread;
} VideoState;

struct FFPlayerConfiguration {
    int32_t audio_disable = false;
    int32_t video_disable = false;
    int32_t subtitle_disable = false;

    int32_t seek_by_bytes = false;

    int32_t show_status = true;

    int64_t start_time = 0;
    int32_t loop = 1;
};


struct CPlayer {
public:
    const std::unique_ptr<PacketQueue> audio_pkt_queue = std::unique_ptr<PacketQueue>(new PacketQueue);
    const std::unique_ptr<PacketQueue> video_pkt_queue = std::unique_ptr<PacketQueue>(new PacketQueue);
    const std::unique_ptr<PacketQueue> subtitle_pkt_queue = std::unique_ptr<PacketQueue>(new PacketQueue);

    const std::unique_ptr<ClockContext> clock_context = std::unique_ptr<ClockContext>(new ClockContext);

    DataSource *data_source = nullptr;

    const std::unique_ptr<DecoderContext> decoder_context = std::unique_ptr<DecoderContext>(new DecoderContext);

    const std::unique_ptr<AudioRender> audio_render = std::unique_ptr<AudioRender>(new AudioRender);
    const std::unique_ptr<VideoRender> video_render = std::unique_ptr<VideoRender>(new VideoRender);


public:

    CPlayer();

    ~CPlayer();


public:

    VideoState *is;
    int audio_disable = 0;
    int video_disable = 0;
    int subtitle_disable = 0;

    char *wanted_stream_spec[AVMEDIA_TYPE_NB]{};
    int seek_by_bytes = -1;

    int show_status = -1;
    int64_t start_time = AV_NOPTS_VALUE;
    int64_t duration = AV_NOPTS_VALUE;
    int fast = 0;
    int genpts = 0;
    int lowres = 0;
    int decoder_reorder_pts = -1;

    int loop = 1;
    int framedrop = -1;
    int infinite_buffer = -1;
    enum ShowMode show_mode = SHOW_MODE_NONE;
    const char *audio_codec_name = nullptr;
    const char *subtitle_codec_name = nullptr;
    const char *video_codec_name = nullptr;
    double rdftspeed = 0.02;


#if CONFIG_AVFILTER
    const char **vfilters_list = NULL;
    int nb_vfilters = 0;
    char *afilters = NULL;
#endif
    int autorotate = 1;
    int find_stream_info = 1;
    int filter_nbthreads = 0;

    /* current context */
    int64_t audio_callback_time = 0;

    SDL_AudioDeviceID audio_dev = 0;
//    SDL_RendererInfo renderer_info;

    void (*on_load_metadata)(void *player) = nullptr;

    // buffered position in seconds. -1 if not avalible
    int64_t buffered_position = -1;

    FFPlayerMessageQueue msg_queue;

    void (*on_message)(struct CPlayer *player, int what, int64_t arg1, int64_t arg2) = nullptr;

    SDL_Thread *msg_tid = nullptr;
    FFPlayerState state = FFP_STATE_IDLE;
    int64_t last_io_buffering_ts = -1;

#ifdef _FLUTTER
    Dart_Port message_send_port;
#endif

    bool paused = false;
};

FFPLAYER_EXPORT void ffplayer_global_init(void *arg);

FFPLAYER_EXPORT CPlayer *ffp_create_player(FFPlayerConfiguration *config);

FFPLAYER_EXPORT void ffplayer_free_player(CPlayer *player);

FFPLAYER_EXPORT void ffp_attach_video_render(CPlayer *player, FFP_VideoRenderCallback *render_callback);

FFPLAYER_EXPORT int ffplayer_open_file(CPlayer *player, const char *filename);

FFPLAYER_EXPORT bool ffplayer_is_paused(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_toggle_pause(CPlayer *player);

FFPLAYER_EXPORT bool ffplayer_is_mute(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_set_mute(CPlayer *player, bool mute);

FFPLAYER_EXPORT int ffplayer_get_current_chapter(CPlayer *player);

FFPLAYER_EXPORT int ffplayer_get_chapter_count(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_seek_to_chapter(CPlayer *player, int chapter);

FFPLAYER_EXPORT double ffplayer_get_current_position(CPlayer *player);

FFPLAYER_EXPORT void ffplayer_seek_to_position(CPlayer *player, double position);

FFPLAYER_EXPORT double ffplayer_get_duration(CPlayer *player);

/**
 * @param volume from 0 to 100.
 */
FFPLAYER_EXPORT void ffp_set_volume(CPlayer *player, int volume);

FFPLAYER_EXPORT int ffp_get_volume(CPlayer *player);

FFPLAYER_EXPORT int ffp_get_state(CPlayer *player);

FFPLAYER_EXPORT void
ffp_set_message_callback(CPlayer *player, void (*callback)(CPlayer *, int32_t, int64_t, int64_t));

FFPLAYER_EXPORT void ffp_refresh_texture(CPlayer *player, void(*on_locked)(VideoRender *video_render_ctx));

/**
 * @return -1 if player invalid; -2 if render_context invalid; 0 if none picture rendered.
 */
FFPLAYER_EXPORT double ffp_get_video_aspect_ratio(CPlayer *player);

#ifdef _FLUTTER

/**
 * Set Message callback for dart. Post message to dart isolate by @param send_port.
 */
FFPLAYER_EXPORT void ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port);

/**
 * Attach flutter render texture.
 *
 * @return the texture id which attached.
 */
FFPLAYER_EXPORT int64_t ffp_attach_video_render_flutter(CPlayer *player);

FFPLAYER_EXPORT void ffp_detach_video_render_flutter(CPlayer *player);
#endif

static inline void ffp_send_msg2(CPlayer *player, int what, int arg1, int arg2) {
    FFPlayerMessage msg = {0};
    msg.what = what;
    msg.arg1 = arg1;
    msg.arg2 = arg2;
    msg.next = nullptr;
    player->msg_queue.Put(&msg);
}

static inline void ffp_send_msg1(CPlayer *player, int what, int64_t arg1) {
    ffp_send_msg2(player, what, arg1, 0);
}

static inline void ffp_send_msg(CPlayer *player, int what) {
    ffp_send_msg1(player, what, 0);
}


#endif  // FFPLAYER_FFPLAYER_H_