//
// Created by yangbin on 2021/4/7.
//

#if 0
#include "base/logging.h"

#include "sdl/sdl_audio_render.h"

namespace media {

/* Minimum SDL audio buffer size, in samples. */
const static auto SDL_AUDIO_MIN_BUFFER_SIZE = 512;
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
const static auto SDL_AUDIO_MAX_CALLBACKS_PER_SEC = 30;

void SdlAudioRender::StartPlaying() {
  SDL_PauseAudioDevice(audio_device_id_, 0);
}

void SdlAudioRender::Flush() {

}

void SdlAudioRender::DoInitialize(PipelineStatusCallback init_callback) {
  auto buffer_size = OpenAudioDevice();
  if (buffer_size <= 0) {
    std::move(init_callback)(AUDIO_RENDERER_ERROR);
    return;
  }
  DCHECK_GT(audio_device_id_, 0);
  init_callback(PIPELINE_OK);
}

int SdlAudioRender::OpenAudioDevice() {
  SDL_AudioSpec wanted_spec, spec;
  static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
  static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
  int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

  auto stream_channel_layout = stream_->audio_decoder_config().channel_layout();
  auto wanted_nb_channels = ChannelLayoutToChannelCount(stream_channel_layout);
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = stream_->audio_decoder_config().samples_per_second();
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
    // TODO read buffered data.
    auto *render = static_cast<SdlAudioRender *>(userdata);
//    render->ReadAudioData(stream, len);
  };
  wanted_spec.userdata = this;
  if (!(audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
      SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
    DLOG(WARNING) << "SDL_OpenAudio ("
                  << stream_channel_layout << " channels,  "
                  << wanted_spec.freq << "Hz): "
                  << SDL_GetError();
    return -1;
  }

  DCHECK_EQ(spec.format, AUDIO_S16SYS);
  return spec.size;
}

} // namespace media

#endif