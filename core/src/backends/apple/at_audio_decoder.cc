#define LOG_TAG "at_audio"

#include "backends/apple/at_audio_decoder.h"

#include <vector>

#include "base/log.h"

namespace lychee {

namespace {

constexpr int kMaxConsecutiveErrors = 10;
// Input() has no packet: the converter returns what it has decoded.
constexpr OSStatus kNoMoreInput = 'lynd';
// Output frames per AudioConverterFillComplexBuffer call.
constexpr UInt32 kChunkFrames = 4096;

void AppendDescriptor(uint8_t tag,
                      const std::vector<uint8_t>& body,
                      std::vector<uint8_t>* out) {
  out->push_back(tag);
  // Four-byte expandable length, as MP4 writers store it.
  size_t size = body.size();
  for (int shift = 21; shift > 0; shift -= 7) {
    out->push_back(static_cast<uint8_t>(0x80 | ((size >> shift) & 0x7f)));
  }
  out->push_back(static_cast<uint8_t>(size & 0x7f));
  out->insert(out->end(), body.begin(), body.end());
}

// The magic cookie AudioToolbox takes for AAC: an ES_Descriptor (the body
// of an MP4 esds box, ISO/IEC 14496-1) around the AudioSpecificConfig.
std::vector<uint8_t> AacCookie(const std::vector<uint8_t>& config) {
  std::vector<uint8_t> specific;
  AppendDescriptor(0x05, config, &specific);
  // objectTypeIndication 0x40 (MPEG-4 audio), streamType 5 (audio) with
  // the reserved bit, bufferSizeDB, maxBitrate, avgBitrate.
  std::vector<uint8_t> decoder_config = {0x40, 0x15, 0, 0, 0, 0, 0, 0, 0,
                                         0,    0,    0, 0};
  decoder_config.insert(decoder_config.end(), specific.begin(), specific.end());
  std::vector<uint8_t> es = {0, 0, 0};  // ES_ID, flags.
  AppendDescriptor(0x04, decoder_config, &es);
  AppendDescriptor(0x06, {0x02}, &es);  // SLConfigDescriptor, predefined.
  std::vector<uint8_t> cookie;
  AppendDescriptor(0x03, es, &cookie);
  return cookie;
}

}  // namespace

ATAudioDecoder::~ATAudioDecoder() {
  if (converter_) {
    AudioConverterDispose(converter_);
  }
}

bool ATAudioDecoder::Initialize(const AudioConfig& config,
                                const AudioOutputFormat& output_format,
                                std::string* error) {
  output_format_ = output_format;
  config_ = config;
  if (config.codec != AudioCodec::kAAC && config.codec != AudioCodec::kALAC) {
    *error = "audiotoolbox: no decoder for " + config.codec_name;
    return false;
  }
  // AAC in ADTS (MPEG-TS) has no extradata: the converter is made from
  // the first packet's header.
  if (config.codec == AudioCodec::kAAC && config.extradata.empty()) {
    return true;
  }
  if (!CreateConverter(config.extradata)) {
    *error = "audiotoolbox: cannot decode " + config.codec_name;
    return false;
  }
  return true;
}

bool ATAudioDecoder::CreateConverter(const std::vector<uint8_t>& extradata) {
  AudioStreamBasicDescription input = {};
  std::vector<uint8_t> cookie;
  if (config_.codec == AudioCodec::kAAC) {
    input.mFormatID = kAudioFormatMPEG4AAC;
    cookie = AacCookie(extradata);
  } else {
    input.mFormatID = kAudioFormatAppleLossless;
    cookie = extradata;
  }
  input.mSampleRate = config_.sample_rate;
  input.mChannelsPerFrame = config_.channels;
  // The cookie knows better: HE-AAC runs at twice the rate the container
  // says, and its first format is the full-quality layer.
  UInt32 size = sizeof(input);
  AudioFormatGetProperty(kAudioFormatProperty_FormatInfo,
                         static_cast<UInt32>(cookie.size()), cookie.data(),
                         &size, &input);
  AudioFormatInfo info = {input, cookie.data(),
                          static_cast<UInt32>(cookie.size())};
  AudioFormatListItem items[8];
  size = sizeof(items);
  if (AudioFormatGetProperty(kAudioFormatProperty_FormatList, sizeof(info),
                             &info, &size, items) == noErr &&
      size >= sizeof(AudioFormatListItem)) {
    input = items[0].mASBD;
  }

  AudioStreamBasicDescription output = {};
  output.mSampleRate = output_format_.sample_rate;
  output.mFormatID = kAudioFormatLinearPCM;
  output.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  output.mChannelsPerFrame = output_format_.channels;
  output.mBitsPerChannel = 32;
  output.mFramesPerPacket = 1;
  output.mBytesPerFrame = 4 * output_format_.channels;
  output.mBytesPerPacket = output.mBytesPerFrame;

  OSStatus status = AudioConverterNew(&input, &output, &converter_);
  if (status != noErr) {
    LOGE("AudioConverterNew failed: %d", static_cast<int>(status));
    converter_ = nullptr;
    return false;
  }
  status = AudioConverterSetProperty(
      converter_, kAudioConverterDecompressionMagicCookie,
      static_cast<UInt32>(cookie.size()), cookie.data());
  if (status != noErr) {
    LOGE("the codec configuration is not valid: %d", static_cast<int>(status));
    return false;
  }
  LOGI("decoder opened for %s %.0f Hz %u ch -> %d Hz %d ch",
       config_.codec_name.c_str(), input.mSampleRate, input.mChannelsPerFrame,
       output_format_.sample_rate, output_format_.channels);
  return true;
}

OSStatus ATAudioDecoder::Input(AudioConverterRef,
                               UInt32* packets,
                               AudioBufferList* data,
                               AudioStreamPacketDescription** descriptions,
                               void* decoder) {
  auto* self = static_cast<ATAudioDecoder*>(decoder);
  if (!self->pending_) {
    *packets = 0;
    // No packets with noErr ends the stream: the converter drains.
    return self->draining_ ? noErr : kNoMoreInput;
  }
  const EncodedPacket* packet = self->pending_;
  self->pending_ = nullptr;
  data->mNumberBuffers = 1;
  size_t skip = self->AdtsHeaderSize(*packet);
  auto bytes = static_cast<UInt32>(packet->size() - skip);
  data->mBuffers[0].mData = const_cast<uint8_t*>(packet->buffer->data() + skip);
  data->mBuffers[0].mDataByteSize = bytes;
  data->mBuffers[0].mNumberChannels = 0;
  self->description_ = {0, 0, bytes};
  if (descriptions) {
    *descriptions = &self->description_;
  }
  *packets = 1;
  return noErr;
}

bool ATAudioDecoder::Decode(const EncodedPacket& packet,
                            const OutputCallback& output) {
  if (packet.end_of_stream) {
    if (!converter_) {
      return true;
    }
    draining_ = true;
  } else if (packet.buffer && packet.size() > 0) {
    if (!converter_) {
      // ADTS: the AudioSpecificConfig from the header's object type,
      // sampling frequency index and channel configuration.
      const uint8_t* adts = packet.buffer->data();
      if (AdtsHeaderSize(packet) == 0) {
        LOGE("AAC without a configuration or ADTS headers");
        return false;
      }
      int object_type = (adts[2] >> 6) + 1;
      int frequency = (adts[2] >> 2) & 0xf;
      int channels = ((adts[2] & 1) << 2) | (adts[3] >> 6);
      std::vector<uint8_t> config = {
          static_cast<uint8_t>((object_type << 3) | (frequency >> 1)),
          static_cast<uint8_t>(((frequency & 1) << 7) | (channels << 3))};
      if (!CreateConverter(config)) {
        return false;
      }
    }
    pending_ = &packet;
    if (next_pts_ == kNoTimestamp) {
      next_pts_ = packet.pts;
    }
  } else {
    return true;
  }
  for (;;) {
    auto buffer = std::make_shared<AudioBuffer>();
    buffer->sample_rate = output_format_.sample_rate;
    buffer->channels = output_format_.channels;
    buffer->samples.resize(static_cast<size_t>(kChunkFrames) *
                           output_format_.channels);
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = output_format_.channels;
    list.mBuffers[0].mDataByteSize =
        static_cast<UInt32>(buffer->samples.size() * sizeof(float));
    list.mBuffers[0].mData = buffer->samples.data();
    UInt32 frames = kChunkFrames;
    OSStatus status = AudioConverterFillComplexBuffer(converter_, Input, this,
                                                      &frames, &list, nullptr);
    if (frames > 0) {
      consecutive_errors_ = 0;
      buffer->samples.resize(static_cast<size_t>(frames) *
                             output_format_.channels);
      buffer->pts = next_pts_;
      if (next_pts_ != kNoTimestamp) {
        next_pts_ += buffer->duration();
      }
      output(std::move(buffer));
    }
    if (status == kNoMoreInput || (status == noErr && frames == 0)) {
      break;
    }
    if (status != noErr) {
      LOGW("AudioConverterFillComplexBuffer: %d", static_cast<int>(status));
      pending_ = nullptr;
      // The converter may be left mid-packet.
      AudioConverterReset(converter_);
      consecutive_errors_++;
      break;
    }
  }
  pending_ = nullptr;
  if (draining_) {
    draining_ = false;
    AudioConverterReset(converter_);
    next_pts_ = kNoTimestamp;
  }
  return consecutive_errors_ < kMaxConsecutiveErrors;
}

size_t ATAudioDecoder::AdtsHeaderSize(const EncodedPacket& packet) const {
  const uint8_t* data = packet.buffer->data();
  if (config_.codec != AudioCodec::kAAC || packet.size() < 7 ||
      data[0] != 0xff || (data[1] & 0xf6) != 0xf0) {
    return 0;
  }
  // Nine bytes with a CRC (protection_absent clear).
  size_t size = (data[1] & 1) ? 7 : 9;
  return size < packet.size() ? size : 0;
}

void ATAudioDecoder::Flush() {
  if (converter_) {
    AudioConverterReset(converter_);
  }
  pending_ = nullptr;
  draining_ = false;
  next_pts_ = kNoTimestamp;
  consecutive_errors_ = 0;
}

}  // namespace lychee
