#include "media/pcm_audio_decoder.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
namespace lychee {
bool PcmAudioDecoder::Initialize(const AudioConfig& config,
                                 const AudioOutputFormat& format,
                                 std::string* error) {
  if (config.codec != AudioCodec::kPCM || config.extradata.size() != 3 ||
      config.channels < 1 || config.channels > 32 || config.sample_rate < 1 ||
      config.sample_rate > 768000 || format.channels < 1 ||
      format.channels > 32 || format.sample_rate < 1 ||
      format.sample_rate > 768000) {
    *error = "pcm: unsupported sample format";
    return false;
  }
  bits_ = config.extradata[0];
  float_ = config.extradata[1] != 0;
  unsigned_ = config.extradata[2] != 0;
  if ((float_ && bits_ != 32 && bits_ != 64) ||
      (!float_ && bits_ != 8 && bits_ != 16 && bits_ != 24 && bits_ != 32)) {
    *error = "pcm: unsupported sample width";
    return false;
  }
  config_ = config;
  format_ = format;
  matrix_ = ChannelMatrix(config.channels, format.channels);
  if (config.sample_rate != format.sample_rate)
    resampler_ = std::make_unique<Resampler>(
        format.channels, config.sample_rate, format.sample_rate);
  return true;
}
bool PcmAudioDecoder::Decode(const EncodedPacket& packet,
                             const OutputCallback& output) {
  std::vector<float> samples;
  if (packet.end_of_stream) {
    if (resampler_) resampler_->Drain(&samples);
  } else if (packet.buffer) {
    const size_t width = static_cast<size_t>(bits_ / 8);
    const size_t block = width * config_.channels;
    if (packet.size() % block) return false;
    const size_t count = packet.size() / block;
    if (count > size_t{1024} * 1024) return false;
    if (anchor_ == kNoTimestamp) anchor_ = packet.pts;
    std::vector<float> mixed(count * format_.channels, 0);
    const uint8_t* data = packet.buffer->data();
    for (size_t frame = 0; frame < count; ++frame) {
      for (int channel = 0; channel < config_.channels; ++channel) {
        const uint8_t* p = data + (frame * config_.channels + channel) * width;
        uint64_t value = 0;
        for (size_t j = 0; j < width; ++j)
          value |= uint64_t(p[j]) << (8 * j);
        double sample;
        if (float_) {
          sample = bits_ == 32
                       ? std::bit_cast<float>(static_cast<uint32_t>(value))
                       : std::bit_cast<double>(value);
          if (!std::isfinite(sample)) sample = 0;
        } else if (unsigned_)
          sample = (double(value) - 128) / 128;
        else {
          const int64_t signed_value =
              (value & (uint64_t(1) << (bits_ - 1)))
                  ? static_cast<int64_t>(value) - (int64_t(1) << bits_)
                  : static_cast<int64_t>(value);
          sample = double(signed_value) / double(uint64_t(1) << (bits_ - 1));
        }
        sample = std::clamp(sample, -1.0, 1.0);
        for (int out = 0; out < format_.channels; ++out)
          mixed[frame * format_.channels + out] +=
              static_cast<float>(sample) *
              matrix_[static_cast<size_t>(out) * config_.channels + channel];
      }
    }
    if (resampler_)
      resampler_->Push(mixed.data(), static_cast<int>(count), &samples);
    else
      samples.swap(mixed);
  }
  if (!samples.empty()) {
    auto buffer = std::make_shared<AudioBuffer>();
    buffer->sample_rate = format_.sample_rate;
    buffer->channels = format_.channels;
    buffer->pts =
        anchor_ == kNoTimestamp
            ? kNoTimestamp
            : anchor_ + frames_ * kMicrosPerSecond / format_.sample_rate;
    buffer->samples.swap(samples);
    frames_ += buffer->frames();
    output(std::move(buffer));
  }
  if (packet.end_of_stream) Flush();
  return true;
}
void PcmAudioDecoder::Flush() {
  if (resampler_) resampler_->Reset();
  anchor_ = kNoTimestamp;
  frames_ = 0;
}
}  // namespace lychee
