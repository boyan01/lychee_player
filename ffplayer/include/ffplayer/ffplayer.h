// ffplayer.h: 标准系统包含文件的包含文件
// 或项目特定的包含文件。
#ifndef FFPLAYER_FFPLAYER_H_
#define FFPLAYER_FFPLAYER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

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

#include "ffplayer_packet_queue.h"
#include "ffplayer_msg_queue.h"

#ifdef _WIN32
#define FFPLAYER_EXPORT __declspec(dllexport)
#else
#define FFPLAYER_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

#if _FLUTTER

#include "third_party/dart/dart_api_dl.h"
#include "flutter.h"

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

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN 0.900
#define EXTERNAL_CLOCK_SPEED_MAX 1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

static unsigned sws_flags = SWS_BICUBIC;

typedef enum ffplayer_info_type {
    buffered
} ffplayer_info_t;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef enum FFPlayerState_ {
    FFP_STATE_IDLE = 0,
    FFP_STATE_READY,
    FFP_STATE_BUFFERING,
    FFP_STATE_END
} FFPlayerState;

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;       /* clock base */
    double pts_drift; /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial; /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial; /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;      /* presentation timestamp for the frame */
    double duration; /* estimated duration of the frame */
    int64_t pos;     /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    SDL_mutex *mutex;
    SDL_cond *cond;
    PacketQueue *pktq;
} FrameQueue;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    int pkt_serial;
    int finished;
    int packet_pending;
    SDL_cond *empty_queue_cond;
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    SDL_Thread *decoder_tid;
} Decoder;

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

    Clock audclk;
    Clock vidclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue subpq;
    FrameQueue sampq;

    Decoder auddec;
    Decoder viddec;
    Decoder subdec;

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

    enum ShowMode {
        SHOW_MODE_NONE = -1,
        SHOW_MODE_VIDEO = 0,
        SHOW_MODE_WAVES,
        SHOW_MODE_RDFT,
        SHOW_MODE_NB
    } show_mode;
    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;

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
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int eof;

    char *filename;
    int width, height, xleft, ytop;
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

typedef struct CPlayer {
    VideoState *is;
    int audio_disable;
    int video_disable;
    int subtitle_disable;

    int display_disable;

    char *wanted_stream_spec[AVMEDIA_TYPE_NB];
    int seek_by_bytes;

    int show_status;
    int64_t start_time;
    int64_t duration;
    int fast;
    int genpts;
    int lowres;
    int decoder_reorder_pts;

    int loop;
    int framedrop;
    int infinite_buffer;
    enum ShowMode show_mode;
    const char *audio_codec_name;
    const char *subtitle_codec_name;
    const char *video_codec_name;
    double rdftspeed;

#if CONFIG_AVFILTER
    const char **vfilters_list = NULL;
    int nb_vfilters = 0;
    char *afilters = NULL;
#endif
    int autorotate;
    int find_stream_info;
    int filter_nbthreads;

    /* current context */
    int64_t audio_callback_time;

    SDL_AudioDeviceID audio_dev;
    SDL_Renderer *renderer;
    SDL_RendererInfo renderer_info;

    void (*on_load_metadata)(void *player);

    bool first_video_frame_loaded;
    bool first_video_frame_rendered;

    // buffered position in seconds. -1 if not avalible
    int64_t buffered_position;

    FFPlayerMessageQueue msg_queue;

    void (*on_message)(struct CPlayer *player, int what, int64_t arg1, int64_t arg2);

    SDL_Thread *msg_tid;
    FFPlayerState state;
    int64_t last_io_buffering_ts;

#ifdef _FLUTTER
    Dart_Port message_send_port;
#endif

} CPlayer;

FFPLAYER_EXPORT void ffplayer_global_init(void *arg);

FFPLAYER_EXPORT CPlayer *ffplayer_alloc_player();

FFPLAYER_EXPORT void ffplayer_free_player(CPlayer *player);

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
FFPLAYER_EXPORT void ffplayer_set_volume(CPlayer *player, int volume);

/**
 * called to display each frame
 * 
 * \return time for next frame should be schudled.
 */
FFPLAYER_EXPORT double ffplayer_draw_frame(CPlayer *player);

FFPLAYER_EXPORT int ffp_get_state(CPlayer *player);

FFPLAYER_EXPORT void
ffp_set_message_callback(CPlayer *player, void (*callback)(CPlayer *, int, int64_t, int64_t));

#ifdef _FLUTTER

FFPLAYER_EXPORT void
ffp_set_message_callback_dart(CPlayer *player, Dart_Port_DL send_port) {
    player->message_send_port = send_port;
}

#endif

static inline void ffp_send_msg2(CPlayer *player, int what, int arg1, int arg2) {
    FFPlayerMessage msg = {0};
    msg.what = what;
    msg.arg1 = arg1;
    msg.arg2 = arg2;
    msg.next = NULL;
    msg_queue_put(&player->msg_queue, &msg);
}

static inline void ffp_send_msg1(CPlayer *player, int what, int64_t arg1) {
    ffp_send_msg2(player, what, arg1, 0);
}

static inline void ffp_send_msg(CPlayer *player, int what) {
    ffp_send_msg1(player, what, 0);
}

#ifdef __cplusplus
}
#endif

#endif  // FFPLAYER_FFPLAYER_H_