//
// Created by yangbin on 2021/3/6.
//

#include "audio_render_sdl_2.h"

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

void AudioRenderSdl2::Start() const {
  SDL_PauseAudioDevice(audio_device_id_, 0);
}

void AudioRenderSdl2::Pause() const {
  SDL_PauseAudioDevice(audio_device_id_, 1);
}

int AudioRenderSdl2::OpenAudioDevice(int64_t wanted_channel_layout,
                                     int wanted_nb_channels,
                                     int wanted_sample_rate,
                                     AudioParams &device_output) {
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
    auto *render = static_cast<AudioRenderSdl2 *>(userdata);
    render->FetchAudioStream(stream, len);
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

  device_output.fmt = AV_SAMPLE_FMT_S16;
  device_output.freq = spec.freq;
  device_output.channel_layout = wanted_channel_layout;
  device_output.channels = spec.channels;
  device_output.frame_size = av_samples_get_buffer_size(nullptr, device_output.channels, 1,
                                                        device_output.fmt,
                                                    1);
  device_output.bytes_per_sec = av_samples_get_buffer_size(nullptr, device_output.channels,
                                                           device_output.freq,
                                                           device_output.fmt, 1);

  return spec.size;
}
