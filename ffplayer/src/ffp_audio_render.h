//
// Created by boyan on 2021/2/12.
//

#ifndef FFPLAYER_FFP_AUDIO_RENDER_H
#define FFPLAYER_FFP_AUDIO_RENDER_H

#include <cstdint>

#include "render_base.h"
#include "ffp_clock.h"
#include "ffp_frame_queue.h"

extern "C" {
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

class AudioRender : public BaseRender {

 private:
  /* current context */
  int64_t audio_callback_time = 0;

  SDL_AudioDeviceID audio_dev = 0;

  int audio_hw_buf_size = 0;
  uint8_t *audio_buf = nullptr;
  // for resample.
  uint8_t *audio_buf1 = nullptr;

  unsigned int audio_buf1_size = 0;

  bool mute = false;
  int audio_volume = 100;

  double audio_clock_from_pts = NAN;
  int audio_clock_serial = -1;

  AudioParams audio_src{};
  AudioParams audio_tgt{};
  struct SwrContext *swr_ctx = nullptr;

  int audio_write_buf_size = 0;
  int audio_buf_size = 0;
  int audio_buf_index = 0;

  double audio_diff_cum = 0; /* used for AV difference average computation */
  double audio_diff_avg_coef = 0;
  int audio_diff_avg_count = 0;

  double audio_diff_threshold = 0;

  std::shared_ptr<ClockContext> clock_ctx_;

  std::unique_ptr<FrameQueue> sample_queue;

 public:

  int *audio_queue_serial = nullptr;

  bool paused = false;

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

  AudioRender(const std::shared_ptr<PacketQueue> &audio_queue, std::shared_ptr<ClockContext> clock_ctx);

  ~AudioRender();

  int Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate);

  int PushFrame(AVFrame *frame, int pkt_serial);

  void Start() const;

  void Pause() const;

  void Abort() override;

  bool IsMute() const;

  void SetMute(bool _mute);

  /**
   * @param _volume 0 - 100
   */
  void SetVolume(int _volume);

  /**
   * @return 0 - 100
   */
  int GetVolume() const;

};

#endif //FFPLAYER_FFP_AUDIO_RENDER_H
