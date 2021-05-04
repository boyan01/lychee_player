//
// Created by yangbin on 2021/5/3.
//

#include "sdl_audio_renderer_sink.h"

#include "base/logging.h"
#include "base/bind_to_current_loop.h"

namespace media {

const int kSdlAudioMinBufferSize = 512;
const int kSdlAudioMaxCallbacksPerSec = 30;

void SdlAudioRendererSink::Initialize(int wanted_nb_channels, int wanted_sample_rate, RenderCallback *render_callback) {
  DCHECK(render_callback);
  render_callback_ = render_callback;

  hw_audio_buffer_size_ = OpenAudioDevices(wanted_nb_channels, wanted_sample_rate);
  if (hw_audio_buffer_size_ <= 0) {
    DLOG(ERROR) << "Failed to open audio devices." << hw_audio_buffer_size_;
    return;
  }

}

bool SdlAudioRendererSink::SetVolume(double volume) {
  return false;
}

int SdlAudioRendererSink::OpenAudioDevices(int wanted_nb_channels, int wanted_sample_rate) {
  SDL_AudioSpec wanted_spec, spec;
  wanted_spec.channels = wanted_nb_channels;
  wanted_spec.freq = wanted_sample_rate;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.silence = 0;
  wanted_spec.samples = std::max(kSdlAudioMinBufferSize,
                                 2 << av_log2(wanted_spec.freq / kSdlAudioMaxCallbacksPerSec));
  wanted_spec.callback = [](void *userdata, Uint8 *stream, int len) {
    auto *sink = static_cast<SdlAudioRendererSink *>(userdata);
    sink->ReadAudioData(stream, len);
  };
  wanted_spec.userdata = this;

  audio_device_id_ = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
      SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
  if (audio_device_id_ <= 0) {
    return -1;
  }

  if (spec.format != AUDIO_S16SYS) {
    audio_device_id_ = 0;
    return -1;
  }

  sample_rate_ = spec.freq;
  return int(spec.size);
}

void SdlAudioRendererSink::ReadAudioData(Uint8 *stream, int len) {
  DCHECK_GT(sample_rate_, 0) << "Invalid sample rate. " << sample_rate_;
  auto delay = (2.0 * hw_audio_buffer_size_) / sample_rate_;
  render_callback_->Render(delay, stream, len);
}

void SdlAudioRendererSink::Start() {

}

void SdlAudioRendererSink::Play() {
  DCHECK_GT(audio_device_id_, 0);
  SDL_PauseAudioDevice(audio_device_id_, 0);
}

void SdlAudioRendererSink::Pause() {
  DCHECK_GT(audio_device_id_, 0);
  SDL_PauseAudioDevice(audio_device_id_, 1);
}

void SdlAudioRendererSink::Stop() {
  DCHECK_GT(audio_device_id_, 0);
  SDL_CloseAudioDevice(audio_device_id_);
}

}