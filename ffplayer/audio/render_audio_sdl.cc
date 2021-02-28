//
// Created by boyan on 21-2-17.
//

#include "render_audio_sdl.h"


/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30


/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

SdlAudioRender::SdlAudioRender() = default;

void SdlAudioRender::Init(const std::shared_ptr<PacketQueue> &audio_queue, std::shared_ptr<MediaClock> clock_ctx) {
  AudioRenderBase::Init(audio_queue, clock_ctx);
}

int SdlAudioRender::Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) {
  SDL_AudioSpec wanted_spec, spec;
  const char *env;
  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

  env = SDL_getenv("SDL_AUDIO_CHANNELS");
  if (env) {
    wanted_nb_channels = std::atoi(env); // NOLINT(cert-err34-c)
    wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
  }
  if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
    wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
  }
  wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
    av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
    return -1;
  }
  while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
    next_sample_rate_idx--;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                              2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
  wanted_spec.callback = [](void *userdata, Uint8 *stream, int len) {
    auto *render = static_cast<SdlAudioRender *>(userdata);
    render->AudioCallback(stream, len);
  };
  wanted_spec.userdata = this;
  while (!(audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
      SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
           wanted_spec.channels, wanted_spec.freq, SDL_GetError());
    wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
    if (!wanted_spec.channels) {
      wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
      wanted_spec.channels = wanted_nb_channels;
      if (!wanted_spec.freq) {
        av_log(nullptr, AV_LOG_ERROR,
               "No more combinations to try, audio open failed\n");
        return -1;
      }
    }
    wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
  }

  if (spec.format != AUDIO_S16SYS) {
    av_log(nullptr, AV_LOG_ERROR,
           "SDL advised audio format %d is not supported!\n", spec.format);
    return -1;
  }
  if (spec.channels != wanted_spec.channels) {
    wanted_channel_layout = av_get_default_channel_layout(spec.channels);
    if (!wanted_channel_layout) {
      av_log(nullptr, AV_LOG_ERROR,
             "SDL advised channel count %d is not supported!\n", spec.channels);
      return -1;
    }
  }

  audio_tgt.fmt = AV_SAMPLE_FMT_S16;
  audio_tgt.freq = spec.freq;
  audio_tgt.channel_layout = wanted_channel_layout;
  audio_tgt.channels = spec.channels;
  audio_tgt.frame_size = av_samples_get_buffer_size(nullptr, audio_tgt.channels, 1,
                                                    audio_tgt.fmt,
                                                    1);
  audio_tgt.bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_tgt.channels,
                                                       audio_tgt.freq,
                                                       audio_tgt.fmt, 1);
  if (audio_tgt.bytes_per_sec <= 0 || audio_tgt.frame_size <= 0) {
    av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
    return -1;
  }

  audio_src = audio_tgt;

  audio_hw_buf_size = spec.size;
  audio_buf_size = 0;
  audio_buf_index = 0;

  /* init averaging filter */
  audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
  audio_diff_avg_count = 0;

  /* since we do not have a precise anough audio FIFO fullness,
we correct audio sync only if larger than this threshold */
  audio_diff_threshold = audio_hw_buf_size / (double) audio_tgt.bytes_per_sec;

  return spec.size;
}

void SdlAudioRender::Start() const {
  SDL_PauseAudioDevice(audio_device_id_, 0);
}

void SdlAudioRender::Pause() const {
  SDL_PauseAudioDevice(audio_device_id_, 1);
}

bool SdlAudioRender::IsMute() const {
  return mute_;
}

void SdlAudioRender::SetMute(bool mute) {
  mute_ = mute;
}

void SdlAudioRender::SetVolume(int _volume) {
  _volume = av_clip(_volume, 0, 100);
  _volume = av_clip(SDL_MIX_MAXVOLUME * _volume / 100, 0, SDL_MIX_MAXVOLUME);
  audio_volume_ = _volume;
}

int SdlAudioRender::GetVolume() const {
  int volume = audio_volume_ * 100 / SDL_MIX_MAXVOLUME;
  return av_clip(volume, 0, 100);
}

void SdlAudioRender::AudioCallback(Uint8 *stream, int len) {
  audio_callback_time = av_gettime_relative();
  while (len > 0) {
    if (audio_buf_index >= audio_buf_size) {
      auto audio_size = AudioDecodeFrame();
      if (audio_size < 0) {
        /* if error, just output silence */
        audio_buf = nullptr;
        audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / audio_tgt.frame_size * audio_tgt.frame_size;
      } else {
        audio_buf_size = audio_size;
      }
      audio_buf_index = 0;
    }
    auto len_flush = (audio_buf_size - audio_buf_index);
    if (len_flush > len) {
      len_flush = len;
    }

    if (!mute_ && audio_buf && audio_volume_ == SDL_MIX_MAXVOLUME) {
      memcpy(stream, (uint8_t *) audio_buf + audio_buf_index, len_flush);
    } else {
      memset(stream, 0, len_flush);
      if (!mute_ && audio_buf) {
        SDL_MixAudioFormat(stream, (uint8_t *) audio_buf + audio_buf_index, AUDIO_S16SYS, len_flush,
                           audio_volume_);
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
        audio_callback_time / 1000000.0);
    clock_ctx_->GetExtClock()->Sync(clock_ctx_->GetAudioClock());
  }
}

int SdlAudioRender::OnBeforeDecodeFrame() {
#if defined(_WIN32)
  while (sample_queue->NbRemaining() == 0) {
    if ((av_gettime_relative() - audio_callback_time) >
        1000000LL * audio_hw_buf_size / audio_tgt.bytes_per_sec / 2)
      return -1;
    av_usleep(1000);
  }
#endif
  return 0;
}

