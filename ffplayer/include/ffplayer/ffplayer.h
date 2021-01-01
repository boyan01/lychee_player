// ffplayer.h: 标准系统包含文件的包含文件
// 或项目特定的包含文件。
#ifndef FFPLAYER_FFPLAYER_H_
#define FFPLAYER_FFPLAYER_H_

#define log(...) printf(__VA_ARGS__)

#include <string>
#include <memory.h>
extern "C" {
#include "libavformat/avformat.h"
}

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;


typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    MyAVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int64_t duration;
    int abort_request;
    int serial;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;

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

class FFPlayer {
   public:
    FFPlayer(const char* filename);
    ~FFPlayer();

    AVPacket flush_pkt;

   private:
    std::string filename_;

    AVFormatContext* format_ctx_;
    // set 1 to abort io request.
    int abort_request_;

    int audio_stream_index_;
    AVStream* audio_stream_;
    PacketQueue audio_queue_;
    struct AudioParams audio_tgt_;
    struct AudioParams audio_src_;
    SDL_AudioDeviceID audio_device_id_;
    int audio_buf_size_;
    int audio_buf_index_;
    int audio_hw_buf_size_;

    SDL_cond *continue_read_thread_;

    Decoder audio_decoder_;
    Decoder video_decoder_;

    void OpenStream();
    int OpenAudioComponentStream(int stream_index);

    int OpenAudio(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate, struct AudioParams *audio_hw_params);
};

#endif  // FFPLAYER_FFPLAYER_H_