//
// Created by boyan on 21-2-17.
//

#ifndef ANDROID_RENDER_AUDIO_BASE_H
#define ANDROID_RENDER_AUDIO_BASE_H

#include "render_base.h"
#include "media_clock.h"
#include "ffp_frame_queue.h"

extern "C" {
#include "libswresample//swresample.h"
}

namespace media {

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB 20

struct AudioParams {
  int freq;
  int channels;
  int64_t channel_layout;
  enum AVSampleFormat fmt;
  int frame_size;
  int bytes_per_sec;
};

class AudioRenderBase : public BaseRender {

 protected:

  int audio_hw_buf_size = 0;
  uint8_t *audio_buf = nullptr;
  // for resample.
  uint8_t *audio_buf1 = nullptr;

  unsigned int audio_buf1_size = 0;

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

  std::shared_ptr<MediaClock> clock_ctx_;

  std::unique_ptr<FrameQueue> sample_queue;

 public:

  int *audio_queue_serial = nullptr;

 private:

  bool paused_ = false;

 protected:
  /**
  * Decode one audio frame and return its uncompressed size.
  *
  * The processed audio frame is decoded, converted if required, and
  * stored in is->audio_buf, with size in bytes given by the return
  * value.
  */
  int AudioDecodeFrame();

  virtual int OnBeforeDecodeFrame();

  int SynchronizeAudio(int nb_samples);

  virtual void OnStart() const = 0;

  virtual void onStop() const = 0;

 public:

  AudioRenderBase();

  virtual void Init(const std::shared_ptr<PacketQueue> &audio_queue, std::shared_ptr<MediaClock> clock_ctx);

  ~AudioRenderBase() override;

  virtual int Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) = 0;

  int PushFrame(AVFrame *frame, int pkt_serial);

  void Abort() override;

  virtual bool IsMute() const = 0;

  virtual void SetMute(bool _mute) = 0;

  /**
   * @param _volume 0 - 100
   */
  virtual void SetVolume(int _volume) = 0;

  /**
   * @return 0 - 100
   */
  virtual int GetVolume() const = 0;

  void Start();

  void Stop();

};

}

#endif //ANDROID_RENDER_AUDIO_BASE_H
