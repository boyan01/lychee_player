//
// Created by yangbin on 2021/3/6.
//

#include "audio_render_oboe.h"

namespace media {

void AudioRenderOboe::OnStart() const {
  if (!audio_stream_) {
    return;
  }
  audio_stream_->requestStart();
}
void AudioRenderOboe::onStop() const {
  if (!audio_stream_) {
    return;
  }
  audio_stream_->requestPause();
}

int AudioRenderOboe::OpenAudioDevice(int64_t wanted_channel_layout,
                                     int wanted_nb_channels,
                                     int wanted_sample_rate,
                                     AudioParams &device_output) {
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

  device_output.fmt = AV_SAMPLE_FMT_S16;
  device_output.freq = audio_stream_->getSampleRate();
  device_output.channel_layout = wanted_channel_layout;
  device_output.channels = audio_stream_->getChannelCount();
  device_output.frame_size = av_samples_get_buffer_size(nullptr, device_output.channels, 1,
                                                        device_output.fmt, 1);
  device_output.bytes_per_sec = av_samples_get_buffer_size(nullptr, device_output.channels,
                                                           device_output.freq,
                                                           device_output.fmt, 1);
  return audio_stream_->getBufferSizeInFrames();
}

AudioRenderOboe::AudioRenderOboe() = default;

AudioRenderOboe::~AudioRenderOboe() {
  audio_stream_->stop();
  audio_stream_->close();
}

oboe::DataCallbackResult AudioRenderOboe::onAudioReady(
    oboe::AudioStream *audioStream,
    void *audioData,
    int32_t numFrames
) {
  unsigned int len = numFrames * audioStream->getChannelCount() * sizeof(int16_t);
  memset(audioData, 0, len);

  {
    auto *outputData = static_cast<uint8_t *>(audioData);
    ReadAudioData(outputData, int(len));
  }
  return oboe::DataCallbackResult::Continue;
}

} // namespace media
