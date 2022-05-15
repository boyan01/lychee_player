//
// Created by Bin Yang on 2022/5/4.
//

#include "sdl_audio_renderer.h"

namespace lychee {

const int kSdlAudioMinBufferSize = 512;
const int kSdlAudioMaxCallbacksPerSec = 30;

SdlAudioRenderer::SdlAudioRenderer(const media::TaskRunner &task_runner, AudioRendererHost *host) : AudioRenderer(
    task_runner,
    host), audio_device_id_(0), is_playing_(false) {}

void SdlAudioRenderer::Play() {
  if (is_playing_) {
    return;
  }
  is_playing_ = true;
  LOG_IF(WARNING, audio_device_id_ == 0) << "audio device is not opened";
  DLOG(INFO) << "SdlAudioRenderer::Play";
  SDL_PauseAudioDevice(audio_device_id_, 0);

}
void SdlAudioRenderer::Pause() {
  if (!is_playing_) {
    return;
  }
  is_playing_ = false;
  LOG_IF(WARNING, audio_device_id_ == 0) << "audio device is not opened";
  DLOG(INFO) << "SdlAudioRenderer::Pause";
  SDL_PauseAudioDevice(audio_device_id_, 1);
}

bool SdlAudioRenderer::OpenDevice(const AudioDecodeConfig &decode_config) {
  SDL_AudioSpec wanted_spec, spec;
  wanted_spec.channels = decode_config.channels();
  wanted_spec.freq = decode_config.samples_per_second();
  wanted_spec.silence = 0;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.samples = std::max(kSdlAudioMinBufferSize,
                                 2 << av_log2(wanted_spec.freq / kSdlAudioMaxCallbacksPerSec));
  wanted_spec.callback = [](void *userdata, Uint8 *stream, int len) {
    auto *renderer = static_cast<SdlAudioRenderer *>(userdata);
    renderer->ReadAudioData(stream, len);
  };
  wanted_spec.userdata = this;

  audio_device_id_ = SDL_OpenAudioDevice(
      nullptr, 0, &wanted_spec, &spec,
      SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
  if (audio_device_id_ == 0) {
    return false;
  }

  audio_device_info_.fmt = AV_SAMPLE_FMT_S16;
  audio_device_info_.channels = spec.channels;
  audio_device_info_.channel_layout = (int) decode_config.channel_layout();
  audio_device_info_.freq = spec.freq;
  audio_device_info_.frame_size = av_samples_get_buffer_size(
      nullptr, audio_device_info_.channels, 1, audio_device_info_.fmt, 1);
  audio_device_info_.bytes_per_sec = av_samples_get_buffer_size(
      nullptr, audio_device_info_.channels, audio_device_info_.freq,
      audio_device_info_.fmt, 1);

  if (spec.format != AUDIO_S16SYS) {
    return false;
  }

  if (is_playing_) {
    SDL_PauseAudioDevice(audio_device_id_, 0);
  }

  return true;
}

void SdlAudioRenderer::ReadAudioData(Uint8 *stream, int length) {
  DCHECK_GT(audio_device_info_.freq, 0) << "Audio device sample invalid";
  auto read = 0;
  while (read < length) {
    auto len = Render(0, stream + read, length - read);
    if (len <= 0) {
      break;
    }
    read += len;
    // only read once.
    break;
  }
  if (read < length) {
    memset(stream + read, 0, length - read);
  }
}

SdlAudioRenderer::~SdlAudioRenderer() {
  if (audio_device_id_ != 0) {
    SDL_LockAudioDevice(audio_device_id_);
    SDL_CloseAudioDevice(audio_device_id_);
    SDL_UnlockAudioDevice(audio_device_id_);
  }
}

bool SdlAudioRenderer::IsPlaying() const {
  return is_playing_;
}

}