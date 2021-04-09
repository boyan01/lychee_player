#include <__bit_reference>
//
// Created by yangbin on 2021/3/28.
//

#include "base/logging.h"
#include "limits.h"

#include "audio_decoder_config.h"

namespace media {

AudioDecoderConfig::AudioDecoderConfig()
    : codec_(AV_CODEC_ID_NONE),
      bytes_per_channel_(0),
      channel_layout_(CHANNEL_LAYOUT_UNSUPPORTED),
      samples_per_second_(0),
      extra_data_size_(0),
      extra_data_(nullptr) {
}

AudioDecoderConfig::AudioDecoderConfig(AVCodecID codec_id,
                                       int bytes_per_channel,
                                       ChannelLayout channel_layout,
                                       int samples_per_second,
                                       const uint8 *extra_data,
                                       size_t extra_data_size) {
  Initialize(codec_id, bytes_per_channel, channel_layout, samples_per_second,
             extra_data, extra_data_size, true);
}

void AudioDecoderConfig::Initialize(AVCodecID codec_id,
                                    int bytes_per_channel,
                                    ChannelLayout channel_layout,
                                    int samples_per_second,
                                    const uint8 *extra_data,
                                    size_t extra_data_size,
                                    bool record_stats) {
  CHECK((extra_data_size != 0) == (extra_data != nullptr));

  if (record_stats) {
//    UMA_HISTOGRAM_ENUMERATION("Media.AudioCodec", CodecId, kAudioCodecMax + 1);
//    // Fake enum histogram to get exact integral buckets.  Expect to never see
//    // any values over 32 and even that is huge.
//    UMA_HISTOGRAM_ENUMERATION("Media.AudioBitsPerChannel", bits_per_channel,
//                              40);
//    UMA_HISTOGRAM_ENUMERATION("Media.AudioChannelLayout", channel_layout,
//                              CHANNEL_LAYOUT_MAX);
//    AudioSampleRate asr = media::AsAudioSampleRate(samples_per_second);
//    if (asr != kUnexpectedAudioSampleRate) {
//      UMA_HISTOGRAM_ENUMERATION("Media.AudioSamplesPerSecond", asr,
//                                kUnexpectedAudioSampleRate);
//    } else {
//      UMA_HISTOGRAM_COUNTS(
//          "Media.AudioSamplesPerSecondUnexpected", samples_per_second);
//    }
  }

  codec_ = codec_id;
  bytes_per_channel_ = bytes_per_channel;
  channel_layout_ = channel_layout;
  samples_per_second_ = samples_per_second;
  extra_data_size_ = extra_data_size;

  if (extra_data_size_ > 0) {
    extra_data_ = new uint8[extra_data_size_];
    memcpy(extra_data_, extra_data, extra_data_size_);
  } else {
    extra_data_ = nullptr;
  }
}

AudioDecoderConfig::~AudioDecoderConfig() {
  delete[] extra_data_;
}

bool AudioDecoderConfig::IsValidConfig() const {
  return codec_ != AV_CODEC_ID_NONE &&
      channel_layout_ != CHANNEL_LAYOUT_UNSUPPORTED &&
      bytes_per_channel_ > 0 &&
      bytes_per_channel_ <= limits::kMaxBytesPerSample &&
      samples_per_second_ > 0 &&
      samples_per_second_ <= limits::kMaxSampleRate;
}

bool AudioDecoderConfig::Matches(const AudioDecoderConfig &config) const {
  return ((CodecId() == config.CodecId()) &&
      (bytes_per_channel() == config.bytes_per_channel()) &&
      (channel_layout() == config.channel_layout()) &&
      (samples_per_second() == config.samples_per_second()) &&
      (extra_data_size() == config.extra_data_size()) &&
      (!extra_data() || !memcmp(extra_data(), config.extra_data(),
                                extra_data_size())));
}

void AudioDecoderConfig::CopyFrom(const AudioDecoderConfig &audio_config) {
  Initialize(audio_config.CodecId(),
             audio_config.bytes_per_channel(),
             audio_config.channel_layout(),
             audio_config.samples_per_second(),
             audio_config.extra_data(),
             audio_config.extra_data_size(),
             false);
}

AVCodecID AudioDecoderConfig::CodecId() const {
  return codec_;
}

int AudioDecoderConfig::bytes_per_channel() const {
  return bytes_per_channel_;
}

ChannelLayout AudioDecoderConfig::channel_layout() const {
  return channel_layout_;
}

int AudioDecoderConfig::samples_per_second() const {
  return samples_per_second_;
}

uint8 *AudioDecoderConfig::extra_data() const {
  return extra_data_;
}

size_t AudioDecoderConfig::extra_data_size() const {
  return extra_data_size_;
}

}  // namespace media