#define LOG_TAG "mediacodec_audio"

#include "backends/android/media_codec_audio_decoder.h"

#include <media/NdkMediaFormat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "backends/android/media_codec_name.h"
#include "base/log.h"

namespace lychee {

namespace {

// Polling step while waiting for codec buffers.
constexpr int64_t kDequeueTimeoutUs = 10000;
// Waiting for the audio that follows end of stream.
constexpr int64_t kEndOfStreamTimeoutUs = kMicrosPerSecond;
// android.media.AudioFormat's ENCODING_PCM_16BIT and ENCODING_PCM_FLOAT.
constexpr int32_t kPcm16Bit = 2;
constexpr int32_t kPcmFloat = 4;
// What Android's Opus decoder skips after a flush (RFC 7845 recommends
// 80 ms of preroll), in nanoseconds, as csd-2 carries it.
constexpr int64_t kOpusSeekPrerollNs = 80'000'000;

const char* Mime(AudioCodec codec) {
  switch (codec) {
    case AudioCodec::kAAC:
      return "audio/mp4a-latm";
    case AudioCodec::kMP3:
      return "audio/mpeg";
    case AudioCodec::kOpus:
      return "audio/opus";
    case AudioCodec::kFLAC:
      return "audio/flac";
    case AudioCodec::kALAC:
      return "audio/alac";
    case AudioCodec::kAC3:
      return "audio/ac3";
    default:
      return nullptr;
  }
}

void AppendInt64(int64_t value, std::vector<uint8_t>* out) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<uint8_t>(value >> (8 * i)));
  }
}

}  // namespace

MediaCodecAudioDecoder::~MediaCodecAudioDecoder() {
  DeleteCodec();
}

bool MediaCodecAudioDecoder::Initialize(const AudioConfig& config,
                                        const AudioOutputFormat& output_format,
                                        std::string* error) {
  config_ = config;
  output_format_ = output_format;
  mime_ = Mime(config.codec);
  if (!mime_) {
    *error = "mediacodec: no decoder for " + config.codec_name;
    return false;
  }
  const std::vector<uint8_t>& extradata = config.extradata;
  switch (config.codec) {
    case AudioCodec::kAAC:
      // AAC in ADTS has none: the first packet's header makes it.
      csd_ = extradata;
      if (csd_.empty()) {
        return true;
      }
      break;
    case AudioCodec::kFLAC: {
      // The stream marker and STREAMINFO as the last metadata block, as
      // Android's extractor hands FLAC in MP4 to the decoder.
      if (extradata.size() != 34) {
        *error = "mediacodec: FLAC without STREAMINFO";
        return false;
      }
      csd_ = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
      csd_.insert(csd_.end(), extradata.begin(), extradata.end());
      break;
    }
    case AudioCodec::kALAC:
      // The ALACSpecificConfig, without the box header around it.
      if (extradata.size() < 24) {
        *error = "mediacodec: ALAC without a configuration";
        return false;
      }
      csd_.assign(extradata.end() - 24, extradata.end());
      break;
    case AudioCodec::kOpus:
      if (extradata.size() < 19) {
        *error = "mediacodec: Opus without an OpusHead";
        return false;
      }
      csd_ = extradata;
      break;
    default:
      break;
  }
  return CreateCodec(error);
}

bool MediaCodecAudioDecoder::CreateCodec(std::string* error) {
  AMediaCodec* codec = AMediaCodec_createDecoderByType(mime_);
  if (!codec) {
    *error = std::string("mediacodec: no decoder for ") + mime_;
    return false;
  }
  codec_name_ = MediaCodecName(codec);
  int rate = config_.sample_rate;
  if (config_.codec == AudioCodec::kOpus || rate <= 0) {
    rate = config_.codec == AudioCodec::kOpus ? 48000 : 44100;
  }
  const int channels = config_.channels > 0 ? config_.channels : 2;
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime_);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
  // Decoders that cannot output float ignore it and output 16 bits. The
  // key's constant is API 28; the key itself works from 24.
  AMediaFormat_setInt32(format, "pcm-encoding", kPcmFloat);
  if (!csd_.empty()) {
    AMediaFormat_setBuffer(format, "csd-0", csd_.data(), csd_.size());
  }
  if (config_.codec == AudioCodec::kOpus) {
    // The OpusHead's pre-skip, and the seek preroll, in nanoseconds.
    const int pre_skip = csd_[10] | (csd_[11] << 8);
    std::vector<uint8_t> delay;
    std::vector<uint8_t> preroll;
    AppendInt64(pre_skip * 1'000'000'000LL / 48000, &delay);
    AppendInt64(kOpusSeekPrerollNs, &preroll);
    AMediaFormat_setBuffer(format, "csd-1", delay.data(), delay.size());
    AMediaFormat_setBuffer(format, "csd-2", preroll.data(), preroll.size());
  }
  media_status_t status =
      AMediaCodec_configure(codec, format, nullptr, nullptr, 0);
  AMediaFormat_delete(format);
  if (status == AMEDIA_OK) {
    status = AMediaCodec_start(codec);
  }
  if (status != AMEDIA_OK) {
    AMediaCodec_delete(codec);
    *error = "mediacodec: " + codec_name_ +
             " failed to start: " + std::to_string(status);
    return false;
  }
  codec_ = codec;
  had_output_ = false;
  LOGI("%s opened for %s %d Hz %d ch -> %d Hz %d ch", codec_name_.c_str(),
       mime_, rate, channels, output_format_.sample_rate,
       output_format_.channels);
  return true;
}

void MediaCodecAudioDecoder::DeleteCodec() {
  if (codec_) {
    AMediaCodec_stop(codec_);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
}

bool MediaCodecAudioDecoder::Decode(const EncodedPacket& packet,
                                    const OutputCallback& output) {
  if (packet.end_of_stream) {
    if (!codec_) {
      return true;
    }
    if (!QueueInput(nullptr, 0, last_pts_,
                    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM, output)) {
      return false;
    }
    const int64_t deadline = MonotonicMicros() + kEndOfStreamTimeoutUs;
    while (!end_of_stream_ && MonotonicMicros() < deadline) {
      if (!DrainOutput(output)) {
        return false;
      }
      if (!end_of_stream_) {
        usleep(kDequeueTimeoutUs);
      }
    }
    if (resampler_) {
      resampler_->Drain(&resampled_);
      Emit(output);
    }
    // A codec takes no input after end of stream until flushed.
    Flush();
    return true;
  }
  if (!packet.buffer || packet.size() == 0) {
    return true;
  }
  const size_t skip = AdtsHeaderSize(packet);
  if (!codec_ && config_.codec == AudioCodec::kAAC && csd_.empty()) {
    // ADTS: the AudioSpecificConfig from the header's object type,
    // sampling frequency index and channel configuration.
    const uint8_t* adts = packet.buffer->data();
    if (skip == 0) {
      LOGE("AAC without a configuration or ADTS headers");
      return false;
    }
    const int object_type = (adts[2] >> 6) + 1;
    const int frequency = (adts[2] >> 2) & 0xf;
    const int channels = ((adts[2] & 1) << 2) | (adts[3] >> 6);
    csd_ = {static_cast<uint8_t>((object_type << 3) | (frequency >> 1)),
            static_cast<uint8_t>(((frequency & 1) << 7) | (channels << 3))};
    static constexpr int kRates[] = {96000, 88200, 64000, 48000, 44100,
                                     32000, 24000, 22050, 16000, 12000,
                                     11025, 8000,  7350};
    if (frequency < 13) {
      config_.sample_rate = kRates[frequency];
    }
    if (channels > 0) {
      config_.channels = channels == 7 ? 8 : channels;
    }
  }
  if (!codec_) {
    std::string error;
    if (!CreateCodec(&error)) {
      LOGE("%s", error.c_str());
      return false;
    }
  }
  int64_t pts = packet.pts != kNoTimestamp ? packet.pts : packet.dts;
  if (pts == kNoTimestamp) {
    pts = last_pts_ + packet.duration;
  }
  last_pts_ = pts;
  return QueueInput(packet.buffer->data() + skip, packet.size() - skip, pts, 0,
                    output) &&
         DrainOutput(output);
}

bool MediaCodecAudioDecoder::QueueInput(const uint8_t* data,
                                        size_t size,
                                        int64_t pts,
                                        uint32_t flags,
                                        const OutputCallback& output) {
  for (;;) {
    // Input buffers come back only as output buffers are taken.
    if (!DrainOutput(output)) {
      return false;
    }
    ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, kDequeueTimeoutUs);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      continue;
    }
    if (index < 0) {
      LOGE("dequeueInputBuffer failed: %zd", index);
      return false;
    }
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, index, &capacity);
    if (!buffer || capacity < size) {
      LOGE("input buffer of %zu bytes for a %zu-byte packet", capacity, size);
      return false;
    }
    if (size) {
      memcpy(buffer, data, size);
    }
    media_status_t status =
        AMediaCodec_queueInputBuffer(codec_, index, 0, size, pts, flags);
    if (status != AMEDIA_OK) {
      LOGE("queueInputBuffer failed: %d", status);
      return false;
    }
    return true;
  }
}

void MediaCodecAudioDecoder::ReadOutputFormat() {
  AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
  int32_t rate = 0;
  int32_t channels = 0;
  int32_t encoding = kPcm16Bit;
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
  AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
  AMediaFormat_getInt32(format, "pcm-encoding", &encoding);
  LOGI("output format: %s", AMediaFormat_toString(format));
  AMediaFormat_delete(format);
  if (rate <= 0 || channels <= 0) {
    return;
  }
  float_output_ = encoding == kPcmFloat;
  if (channels != channels_) {
    channels_ = channels;
    matrix_ = ChannelMatrix(channels_, output_format_.channels);
  }
  if (rate != rate_) {
    rate_ = rate;
    resampler_.reset();
    if (rate_ != output_format_.sample_rate) {
      resampler_ = std::make_unique<Resampler>(output_format_.channels, rate_,
                                               output_format_.sample_rate);
    }
  }
}

bool MediaCodecAudioDecoder::DrainOutput(const OutputCallback& output) {
  for (;;) {
    AMediaCodecBufferInfo info = {};
    ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      return true;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      had_output_ = true;
      ReadOutputFormat();
      continue;
    }
    if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    if (index < 0) {
      LOGE("dequeueOutputBuffer failed: %zd", index);
      return false;
    }
    had_output_ = true;
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
      end_of_stream_ = true;
    }
    if (rate_ == 0) {
      // Codecs announce their format before the first buffer; some only
      // through the buffer's own format.
      ReadOutputFormat();
    }
    size_t capacity = 0;
    const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, index, &capacity);
    const int sample_bytes = float_output_ ? 4 : 2;
    const int frames = data && channels_ > 0 && info.size > 0
                           ? info.size / (sample_bytes * channels_)
                           : 0;
    if (frames > 0) {
      const size_t samples = static_cast<size_t>(frames) * channels_;
      decoded_.resize(samples);
      const uint8_t* start = data + info.offset;
      if (float_output_) {
        memcpy(decoded_.data(), start, samples * sizeof(float));
      } else {
        for (size_t i = 0; i < samples; ++i) {
          int16_t sample;
          memcpy(&sample, start + 2 * i, sizeof(sample));
          decoded_[i] = sample / 32768.0f;
        }
      }
    }
    AMediaCodec_releaseOutputBuffer(codec_, index, false);
    if (frames == 0) {
      continue;
    }
    if (anchor_pts_ == kNoTimestamp) {
      anchor_pts_ = info.presentationTimeUs;
      frames_out_ = 0;
    }
    const int out_channels = output_format_.channels;
    mixed_.assign(static_cast<size_t>(frames) * out_channels, 0.0f);
    for (int f = 0; f < frames; ++f) {
      const float* in = &decoded_[static_cast<size_t>(f) * channels_];
      float* out = &mixed_[static_cast<size_t>(f) * out_channels];
      for (int o = 0; o < out_channels; ++o) {
        const float* weights = &matrix_[static_cast<size_t>(o) * channels_];
        float sum = 0;
        for (int i = 0; i < channels_; ++i) {
          sum += weights[i] * in[i];
        }
        out[o] = sum;
      }
    }
    if (resampler_) {
      resampler_->Push(mixed_.data(), frames, &resampled_);
    } else {
      resampled_.insert(resampled_.end(), mixed_.begin(), mixed_.end());
    }
    Emit(output);
  }
}

void MediaCodecAudioDecoder::Emit(const OutputCallback& output) {
  if (resampled_.empty()) {
    return;
  }
  auto buffer = std::make_shared<AudioBuffer>();
  buffer->sample_rate = output_format_.sample_rate;
  buffer->channels = output_format_.channels;
  buffer->samples.swap(resampled_);
  resampled_.clear();
  buffer->pts =
      anchor_pts_ + frames_out_ * kMicrosPerSecond / buffer->sample_rate;
  frames_out_ += buffer->frames();
  output(std::move(buffer));
}

size_t MediaCodecAudioDecoder::AdtsHeaderSize(
    const EncodedPacket& packet) const {
  const uint8_t* data = packet.buffer->data();
  if (config_.codec != AudioCodec::kAAC || packet.size() < 7 ||
      data[0] != 0xff || (data[1] & 0xf6) != 0xf0) {
    return 0;
  }
  // Nine bytes with a CRC (protection_absent clear).
  size_t size = (data[1] & 1) ? 7 : 9;
  return size < packet.size() ? size : 0;
}

void MediaCodecAudioDecoder::Flush() {
  if (codec_) {
    if (had_output_) {
      media_status_t status = AMediaCodec_flush(codec_);
      if (status != AMEDIA_OK) {
        LOGW("flush failed: %d", status);
      }
    } else {
      // A flush before any output may drop the configuration: start over.
      DeleteCodec();
      std::string error;
      if (!CreateCodec(&error)) {
        LOGE("%s", error.c_str());
      }
    }
  }
  if (resampler_) {
    resampler_->Reset();
  }
  resampled_.clear();
  anchor_pts_ = kNoTimestamp;
  frames_out_ = 0;
  end_of_stream_ = false;
}

}  // namespace lychee
