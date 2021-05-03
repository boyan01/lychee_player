//
// Created by yangbin on 2021/3/6.
//

#include "audio_render_basic.h"

namespace media {

int BasicAudioRender::Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) {
  int buf_size = OpenAudioDevice(wanted_channel_layout, wanted_nb_channels, wanted_sample_rate, audio_tgt);

  if (buf_size < 0) {
    av_log(nullptr, AV_LOG_ERROR, "Open Audio Devices Failed.\n");
    return buf_size;
  }

  if (audio_tgt.bytes_per_sec <= 0 || audio_tgt.frame_size <= 0) {
    av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
  }

  av_log(nullptr,
         AV_LOG_INFO,
         "open device success. wanted(%d,%d,%d), actual(%d,%d), %d bytes/s frame_size: %d, freq = %d, buf_size = %d \n",
         int(wanted_channel_layout), wanted_nb_channels, wanted_sample_rate,
         int(audio_tgt.channel_layout), audio_tgt.channels,
         audio_tgt.bytes_per_sec, audio_tgt.frame_size, audio_tgt.freq,
         buf_size);

  audio_src = audio_tgt;

  audio_hw_buf_size = buf_size;
  audio_buf_size = 0;
  audio_buf_index = 0;

  audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
  audio_diff_avg_count = 0;

  /* since we do not have a precise enough audio FIFO fullness, we correct audio sync only if larger than this threshold */
  audio_diff_threshold = audio_hw_buf_size / (double) audio_tgt.bytes_per_sec;

  audio_src = audio_tgt;

  // start audio thread
  return buf_size;
}

void BasicAudioRender::ReadAudioData(uint8_t *stream, int len) {
  av_log(nullptr, AV_LOG_DEBUG, "ReadAudioData: len = %d\n", len);

  audio_callback_time_ = av_gettime_relative();
  while (len > 0) {
    if (audio_buf_index >= audio_buf_size) {
      auto audio_size = AudioDecodeFrame();
      if (audio_size < 0) {
        /* if error, just output silence */
        audio_buf = nullptr;
        audio_buf_size = len;
        av_log(nullptr, AV_LOG_DEBUG, "Decode audio frame failed. audio_size = %d , len = %d \n", audio_size, len);
      } else {
        audio_buf_size = audio_size;
      }
      audio_buf_index = 0;
    }
    auto len_flush = (audio_buf_size - audio_buf_index);
    if (len_flush > len) {
      len_flush = len;
    }

    if (!mute_ && audio_buf && audio_volume_ == MAX_AUDIO_VOLUME) {
      memcpy(stream, audio_buf + audio_buf_index, len_flush);
    } else {
      memset(stream, 0, len_flush);
      if (!mute_ && audio_buf) {
        // mix audio volume to stream.
        MixAudioVolume(stream, audio_buf + audio_buf_index, len_flush, audio_volume_);
      }
    }

    len -= len_flush;
    stream += len_flush;
    audio_buf_index += len_flush;
  }

  audio_write_buf_size = audio_buf_size - audio_buf_index;
  if (!isnan(audio_clock_from_pts)) {
    clock_ctx_->GetAudioClock()->SetClockAt(
        audio_clock_from_pts - (double) (2 * audio_hw_buf_size + audio_write_buf_size) /
            audio_tgt.bytes_per_sec, audio_clock_serial,
        audio_callback_time_ / 1000000.0);
    clock_ctx_->GetExtClock()->Sync(clock_ctx_->GetAudioClock());
  }

  NotifyRenderProceed();
}

bool BasicAudioRender::IsMute() const {
  return mute_;
}

void BasicAudioRender::SetMute(bool _mute) {
  mute_ = _mute;
}

void BasicAudioRender::SetVolume(int _volume) {
  audio_volume_ = _volume;
}

int BasicAudioRender::GetVolume() const {
  return audio_volume_;
}

int BasicAudioRender::OnBeforeDecodeFrame() {
#if defined(_WIN32)
//  while (sample_queue->NbRemaining() == 0) {
//    if ((av_gettime_relative() - audio_callback_time_) >
//        1000000LL * audio_hw_buf_size / audio_tgt.bytes_per_sec / 2)
//      return -1;
//    av_usleep(1000);
//  }
#endif
  return 0;
}

#define ADJUST_VOLUME(s, v) ((s) = ((s)*(v))/MAX_AUDIO_VOLUME)

// from SDL_MixAudioFormat#AUDIO_S16LSB
void BasicAudioRender::MixAudioVolume(uint8_t *dst, const uint8_t *src, uint32_t len, int volume) {

  int16_t src1, src2;
  int dst_sample;
  const int max_audioval = ((1 << (16 - 1)) - 1);
  const int min_audioval = -(1 << (16 - 1));

  len /= 2;
  while (len--) {
    src1 = (int16_t) ((src[1]) << 8 | src[0]);
    ADJUST_VOLUME(src1, volume);
    src2 = (int16_t) ((dst[1]) << 8 | dst[0]);
    src += 2;
    dst_sample = src1 + src2;
    if (dst_sample > max_audioval) {
      dst_sample = max_audioval;
    } else if (dst_sample < min_audioval) {
      dst_sample = min_audioval;
    }
    dst[0] = dst_sample & 0xFF;
    dst_sample >>= 8;
    dst[1] = dst_sample & 0xFF;
    dst += 2;
  }
}

bool BasicAudioRender::IsReady() {
  return sample_queue->NbRemaining() > 0 || (audio_buf_size > 0 && audio_buf_index < audio_buf_size);
}

}
