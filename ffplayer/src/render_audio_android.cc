//
// Created by boyan on 2021/2/21.
//
#ifdef _FLUTTER_ANDROID

#include "render_audio_android.h"

namespace media {

AndroidAudioRender::AndroidAudioRender() = default;

AndroidAudioRender::~AndroidAudioRender() = default;

int AndroidAudioRender::Open(int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate) {
  return 0;
}

void AndroidAudioRender::Start() const {

}

void AndroidAudioRender::Pause() const {

}

bool AndroidAudioRender::IsMute() const {
  return false;
}

void AndroidAudioRender::SetMute(bool _mute) {

}

void AndroidAudioRender::SetVolume(int _volume) {

}

int AndroidAudioRender::GetVolume() const {
  return 0;
}

void AndroidAudioRender::Init(const std::shared_ptr<PacketQueue> &audio_queue,
                              std::shared_ptr<ClockContext> clock_ctx) {
  AudioRenderBase::Init(audio_queue, clock_ctx);
}

void AndroidAudioRender::Abort() {
  AudioRenderBase::Abort();
}

int AndroidAudioRender::OnBeforeDecodeFrame() {
  return AudioRenderBase::OnBeforeDecodeFrame();
}

}

#endif // _FLUTTER_ANDROID