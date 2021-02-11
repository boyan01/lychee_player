//
// Created by boyan on 2021/2/12.
//

#ifndef FFPLAYER_FFP_AUDIO_RENDER_H
#define FFPLAYER_FFP_AUDIO_RENDER_H

#include <cstdint>


#include "ffplayer/proto.h"
#include "ffp_clock.h"
#include "ffp_frame_queue.h"

extern "C" {
#include "libavutil/frame.h"
#include "SDL2/SDL.h"
};

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

class AudioRender {

private:
    /* current context */
    int64_t audio_callback_time;

    SDL_AudioDeviceID audio_dev = 0;

    int audio_hw_buf_size;
    uint8_t *audio_buf;
    // for resample.
    uint8_t *audio_buf1;

    unsigned int audio_buf1_size = 0;


    bool mute = false;
    int audio_volume = 100;

    double audio_clock_from_pts;
    int audio_clock_serial;

    bool paused = false;

    AudioParams audio_src{};
    AudioParams audio_tgt{};
    struct SwrContext *swr_ctx = nullptr;

    int audio_write_buf_size;
    int audio_buf_size;
    int audio_buf_index;

    double audio_diff_avg_coef;
    int audio_diff_avg_count;

    double audio_diff_threshold;

public:
    FrameQueue *sample_queue;

    Clock *audio_clock;
    Clock *ext_clock;

    int *audio_queue_serial;


private:
    void AudioCallback(Uint8 *stream, int len);

    /**
     * Decode one audio frame and return its uncompressed size.
     *
     * The processed audio frame is decoded, converted if required, and
     * stored in is->audio_buf, with size in bytes given by the return
     * value.
     */
    int AudioDecodeFrame();

    int SynchronizeAudio(int nb_samples);

public:

    int Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate);

    int PushFrame(AVFrame *frame, int pkt_serial) const;

    void Start() const;

    void Pause() const;


};

#endif //FFPLAYER_FFP_AUDIO_RENDER_H
