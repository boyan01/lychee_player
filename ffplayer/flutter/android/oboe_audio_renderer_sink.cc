//
// Created by yangbin on 2021/5/6.
//

#include "oboe_audio_renderer_sink.h"

#include "base/logging.h"

namespace media {

OboeAudioRendererSink::~OboeAudioRendererSink() {
  if (audio_stream_) {
    audio_stream_->stop(0);
    audio_stream_->close();
  }
}

void OboeAudioRendererSink::Initialize(
    int wanted_nb_channels, int wanted_sample_rate,
    AudioRendererSink::RenderCallback *render_callback) {
  DCHECK(render_callback);

  oboe::AudioStreamBuilder stream_builder;
  stream_builder.setSharingMode(oboe::SharingMode::Shared)
      ->setChannelCount(wanted_nb_channels)
      ->setDirection(oboe::Direction::Output)
      ->setSampleRate(wanted_sample_rate)
      ->setFormat(oboe::AudioFormat::I16)
      ->setDataCallback(this)
      ->setUsage(oboe::Usage::Media)
      ->setContentType(oboe::ContentType::Music)
      ->setPerformanceMode(oboe::PerformanceMode::PowerSaving)
      ->openStream(audio_stream_);

  render_callback_ = render_callback;
}

bool OboeAudioRendererSink::SetVolume(double volume) { return false; }

void OboeAudioRendererSink::Play() {
  if (!audio_stream_) {
    return;
  }
  audio_stream_->requestStart();
}

void OboeAudioRendererSink::Pause() {
  if (!audio_stream_) {
    return;
  }
  audio_stream_->requestPause();
}

oboe::DataCallbackResult OboeAudioRendererSink::onAudioReady(
    oboe::AudioStream *audioStream, void *audioData, int32_t numFrames) {
  int len = numFrames * audioStream->getBytesPerFrame();
  memset(audioData, 0, len);

  {
    auto delay = audioStream->calculateLatencyMillis().value() / 1000;
    auto *outputData = static_cast<uint8_t *>(audioData);
    render_callback_->Render(delay, outputData, len);
  }
  return oboe::DataCallbackResult::Continue;
}

}  // namespace media